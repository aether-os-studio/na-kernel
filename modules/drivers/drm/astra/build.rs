//! Generates Rust register and firmware-data modules from the Linux amdgpu
//! sources used as ASTRA's hardware reference.
//!
//! The reference tree is an immutable, versioned build input.  The build
//! script performs a shallow, sparse clone into Cargo's target directory on
//! first use, so ASTRA does not depend on a separately checked-out Linux tree.

use std::collections::BTreeMap;
use std::env;
use std::error::Error;
use std::fmt::Write;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

type BuildResult<T> = Result<T, Box<dyn Error>>;

const LINUX_VERSION: &str = "7.1.8";
const LINUX_REPOSITORY: &str = "https://github.com/gregkh/linux.git";

struct IpHeader {
    /// Rust module name under `regs`.
    module: &'static str,
    /// Directory under `drivers/gpu/drm/amd/include/asic_reg`.
    dir: &'static str,
    /// Header file base name (e.g. `gc_10_3_0`).
    base: &'static str,
    has_defaults: bool,
}

const HEADERS: &[IpHeader] = &[
    IpHeader::new("gc10_3_0", "gc", "gc_10_3_0").with_defaults(),
    IpHeader::new("nbio4_3_0", "nbio", "nbio_4_3_0"),
    IpHeader::new("nbio2_3", "nbio", "nbio_2_3"),
    IpHeader::new("mp13_0_2", "mp", "mp_13_0_2"),
    IpHeader::new("osssys6_0_0", "oss", "osssys_6_0_0"),
    IpHeader::new("mmhub2_0_0", "mmhub", "mmhub_2_0_0").with_defaults(),
    IpHeader::new("vcn3_0_0", "vcn", "vcn_3_0_0"),
    IpHeader::new("smuio11_0_0", "smuio", "smuio_11_0_0"),
    IpHeader::new("smuio11_0_6", "smuio", "smuio_11_0_6"),
    IpHeader::new("thm11_0_2", "thm", "thm_11_0_2"),
    IpHeader::new("hdp5_0_0", "hdp", "hdp_5_0_0"),
    IpHeader::new("dcn3_0_2", "dcn", "dcn_3_0_2"),
    IpHeader::new("dpcs3_0_0", "dpcs", "dpcs_3_0_0"),
];

impl IpHeader {
    const fn new(module: &'static str, dir: &'static str, base: &'static str) -> Self {
        Self {
            module,
            dir,
            base,
            has_defaults: false,
        }
    }

    const fn with_defaults(mut self) -> Self {
        self.has_defaults = true;
        self
    }

    fn kinds(&self) -> impl Iterator<Item = HeaderKind> {
        [
            Some(HeaderKind::Offset),
            Some(HeaderKind::ShiftMask),
            self.has_defaults.then_some(HeaderKind::Default),
        ]
        .into_iter()
        .flatten()
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HeaderKind {
    Offset,
    ShiftMask,
    Default,
}

impl HeaderKind {
    const fn suffix(self) -> &'static str {
        match self {
            Self::Offset => "offset",
            Self::ShiftMask => "sh_mask",
            Self::Default => "default",
        }
    }

    const fn rust_type(self) -> &'static str {
        match self {
            Self::ShiftMask => "u64",
            Self::Offset | Self::Default => "u32",
        }
    }
}

/// A temporary directory that removes itself unless atomically promoted into
/// the persistent build cache.
struct TemporaryDirectory {
    path: PathBuf,
    persisted: bool,
}

impl TemporaryDirectory {
    fn new(path: PathBuf) -> io::Result<Self> {
        match fs::symlink_metadata(&path) {
            Ok(metadata) if metadata.file_type().is_dir() => fs::remove_dir_all(&path)?,
            Ok(_) => fs::remove_file(&path)?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
        fs::create_dir(&path)?;
        Ok(Self {
            path,
            persisted: false,
        })
    }

    fn path(&self) -> &Path {
        &self.path
    }

    fn persist(mut self, destination: &Path) -> io::Result<()> {
        fs::rename(&self.path, destination)?;
        self.persisted = true;
        Ok(())
    }
}

impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        if !self.persisted {
            let _ = fs::remove_dir_all(&self.path);
        }
    }
}

/// Versioned Linux source cache owned by the Cargo target directory.
struct LinuxSource {
    root: PathBuf,
}

impl LinuxSource {
    fn acquire(manifest: &Path) -> BuildResult<Self> {
        let target = env::var_os("CARGO_TARGET_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| manifest.join("target"));
        let target = if target.is_absolute() {
            target
        } else {
            manifest.join(target)
        };
        let cache = target.join("astra-sources");
        let root = cache.join(format!("linux-{LINUX_VERSION}"));
        let source_ref = format!("v{LINUX_VERSION}");
        let marker = format!(
            "repository={LINUX_REPOSITORY}\nref={source_ref}\ndepth=1\nsparse=drivers/gpu/drm/amd\n"
        );

        if Self::is_ready(&root, &marker) {
            return Ok(Self { root });
        }

        fs::create_dir_all(&cache)?;
        Self::remove_stale(&root)?;

        let temporary = TemporaryDirectory::new(
            cache.join(format!(".linux-{LINUX_VERSION}.{}.tmp", process::id())),
        )?;
        let status = Command::new("git")
            .args([
                "clone",
                "--depth",
                "1",
                "--filter=blob:none",
                "--sparse",
                "--single-branch",
                "--branch",
            ])
            .arg(&source_ref)
            .arg(LINUX_REPOSITORY)
            .arg(temporary.path())
            .status()?;
        if !status.success() {
            return Err(io::Error::other(format!(
                "failed to shallow-clone Linux {LINUX_VERSION} from {LINUX_REPOSITORY}"
            ))
            .into());
        }
        let status = Command::new("git")
            .arg("-C")
            .arg(temporary.path())
            .args(["sparse-checkout", "set", "drivers/gpu/drm/amd"])
            .status()?;
        if !status.success() {
            return Err(io::Error::other(format!(
                "failed to sparse-checkout Linux {LINUX_VERSION} AMD DRM sources"
            ))
            .into());
        }

        fs::write(temporary.path().join(".astra-source"), &marker)?;
        if !Self::is_ready(temporary.path(), &marker) {
            return Err(io::Error::other(
                "shallow Linux clone is missing the sparse AMD DRM sources",
            )
            .into());
        }

        match temporary.persist(&root) {
            Ok(()) => {}
            // Another concurrent Cargo invocation may have populated the
            // same immutable cache while this one was cloning it.
            Err(error) if Self::is_ready(&root, &marker) => drop(error),
            Err(error) => return Err(error.into()),
        }

        Ok(Self { root })
    }

    fn is_ready(root: &Path, marker: &str) -> bool {
        Self::is_directory(root)
            && Self::is_directory(&root.join(".git"))
            && Self::is_file(&root.join("Makefile"))
            && Self::is_directory(&root.join("drivers/gpu/drm/amd"))
            && Self::is_directory(&root.join("drivers/gpu/drm/amd/include/asic_reg"))
            && Self::is_directory(&root.join("drivers/gpu/drm/amd/amdgpu"))
            && Self::is_file(&root.join(".astra-source"))
            && fs::read_to_string(root.join(".astra-source")).is_ok_and(|value| value == marker)
    }

    /// Cache entries must be real cloned artifacts. Following a symlink
    /// here would silently reintroduce the old dependency on an external Linux
    /// checkout and would also let a partially replaced cache appear valid.
    fn is_directory(path: &Path) -> bool {
        fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_dir())
    }

    fn is_file(path: &Path) -> bool {
        fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_file())
    }

    fn remove_stale(path: &Path) -> io::Result<()> {
        match fs::symlink_metadata(path) {
            Ok(metadata) if metadata.file_type().is_dir() => fs::remove_dir_all(path),
            Ok(_) => fs::remove_file(path),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
        }
    }

    fn asic_registers(&self) -> PathBuf {
        self.root.join("drivers/gpu/drm/amd/include/asic_reg")
    }

    fn amdgpu(&self) -> PathBuf {
        self.root.join("drivers/gpu/drm/amd/amdgpu")
    }
}

struct CHeader {
    path: PathBuf,
    text: String,
}

impl CHeader {
    fn read(path: PathBuf) -> BuildResult<Self> {
        let text = fs::read_to_string(&path).map_err(|error| {
            io::Error::new(
                error.kind(),
                format!("cannot read Linux header {}: {error}", path.display()),
            )
        })?;
        println!("cargo:rerun-if-changed={}", path.display());
        Ok(Self { path, text })
    }

    /// Extracts `#define NAME VALUE` pairs; the first definition wins,
    /// mirroring the `#ifndef` guards in the Linux headers.
    fn defines(&self) -> BTreeMap<String, u64> {
        let mut defines = BTreeMap::new();
        for line in self.text.lines() {
            let Some(rest) = line.trim().strip_prefix("#define ") else {
                continue;
            };
            let mut parts = rest.split_whitespace();
            let (Some(name), Some(value)) = (parts.next(), parts.next()) else {
                continue;
            };
            let is_register = name.starts_with("mm") || name.starts_with("reg");
            let is_field = name.ends_with("__SHIFT") || name.ends_with("_MASK");
            if !is_register && !is_field {
                continue;
            }
            if let Some(value) = Self::value(value) {
                defines.entry(name.to_owned()).or_insert(value);
            }
        }
        defines
    }

    /// Extracts static `unsigned int`/`u32` arrays used by gfx10.
    fn arrays(&self) -> BTreeMap<String, Vec<u64>> {
        let mut arrays = BTreeMap::new();
        let lines: Vec<_> = self.text.lines().collect();
        let mut index = 0;
        while index < lines.len() {
            let line = lines[index].trim();
            if !line.contains("static const")
                || !(line.contains("unsigned int") || line.contains(" u32 "))
            {
                index += 1;
                continue;
            }
            let Some(bracket) = line.find('[') else {
                index += 1;
                continue;
            };
            let name = line[..bracket]
                .rsplit(|character: char| !character.is_ascii_alphanumeric() && character != '_')
                .next()
                .unwrap_or("")
                .trim();
            if name.is_empty()
                || !name
                    .chars()
                    .next()
                    .is_some_and(|character| character.is_ascii_alphabetic() || character == '_')
            {
                index += 1;
                continue;
            }

            let mut body = String::new();
            let mut found_open = line.contains('{');
            while index < lines.len() {
                let current = lines[index];
                body.push_str(current);
                body.push('\n');
                if found_open && (current.trim_end().ends_with("};") || current.contains('}')) {
                    break;
                }
                found_open |= current.contains('{');
                index += 1;
            }
            if let Some(values) = Self::array_values(&body) {
                arrays.insert(name.to_owned(), values);
            }
            index += 1;
        }
        arrays
    }

    fn value(raw: &str) -> Option<u64> {
        let raw = raw.trim().trim_end_matches(['u', 'U', 'l', 'L']);
        raw.strip_prefix("0x")
            .map_or_else(|| raw.parse().ok(), |hex| u64::from_str_radix(hex, 16).ok())
    }

    fn array_values(body: &str) -> Option<Vec<u64>> {
        let open = body.find('{')?;
        let close = body[open + 1..].rfind('}')?;
        let values = Self::without_comments(&body[open + 1..open + 1 + close])
            .split(',')
            .filter_map(|token| Self::value(token.trim()))
            .collect::<Vec<_>>();
        (!values.is_empty()).then_some(values)
    }

    /// Removes C/C++ comments before numeric array parsing.  Names in gfx10
    /// comments contain digits that must not become packet data.
    fn without_comments(text: &str) -> String {
        let mut output = String::with_capacity(text.len());
        let mut characters = text.chars().peekable();
        while let Some(character) = characters.next() {
            if character != '/' {
                output.push(character);
                continue;
            }
            match characters.peek().copied() {
                Some('/') => {
                    characters.next();
                    if characters.by_ref().any(|comment| comment == '\n') {
                        output.push('\n');
                    }
                }
                Some('*') => {
                    characters.next();
                    let mut previous = '\0';
                    for comment in characters.by_ref() {
                        if previous == '*' && comment == '/' {
                            break;
                        }
                        previous = comment;
                    }
                }
                _ => output.push(character),
            }
        }
        output
    }
}

struct Generator {
    linux: LinuxSource,
    output: PathBuf,
}

impl Generator {
    fn new(manifest: &Path) -> BuildResult<Self> {
        Ok(Self {
            linux: LinuxSource::acquire(manifest)?,
            output: PathBuf::from(env::var("OUT_DIR")?),
        })
    }

    fn run(&self) -> BuildResult<()> {
        self.registers()?;
        self.gfx_data()?;
        Ok(())
    }

    fn registers(&self) -> BuildResult<()> {
        let source = self.linux.asic_registers();
        let mut modules = String::from("// AUTO-GENERATED by astra build.rs. Do not edit.\n");
        for header in HEADERS {
            let mut includes = String::new();
            for kind in header.kinds() {
                let input = CHeader::read(source.join(header.dir).join(format!(
                    "{}_{}.h",
                    header.base,
                    kind.suffix()
                )))?;
                let file_name = format!("{}_{}.rs", header.base, kind.suffix());
                fs::write(
                    self.output.join(&file_name),
                    Self::emit_defines(&input, kind),
                )?;
                writeln!(
                    includes,
                    "include!(concat!(env!(\"OUT_DIR\"), \"/{file_name}\"));"
                )?;
            }
            writeln!(
                modules,
                "#[allow(dead_code, non_upper_case_globals, clippy::all)]\npub mod {} {{\n{includes}}}",
                header.module
            )?;
        }
        fs::write(self.output.join("astra_regs_mod.rs"), modules)?;
        Ok(())
    }

    fn gfx_data(&self) -> BuildResult<()> {
        let source = self.linux.amdgpu();
        for (name, output) in [
            ("clearstate_gfx10.h", "gfx10_clearstate.rs"),
            ("gfx_v10_0_cleaner_shader.h", "gfx10_cleaner_shader.rs"),
        ] {
            let input = CHeader::read(source.join(name))?;
            fs::write(self.output.join(output), Self::emit_arrays(&input))?;
        }
        Ok(())
    }

    fn emit_defines(header: &CHeader, kind: HeaderKind) -> String {
        let defines = header.defines();
        let rust_type = kind.rust_type();
        let mut output = Self::preamble(&header.path);
        for (name, value) in &defines {
            if rust_type == "u32" && *value > u32::MAX as u64 {
                // Absolute-address cfgBIF defines are not dword offsets.
                continue;
            }
            let _ = writeln!(output, "pub const {name}: {rust_type} = {value};");
        }
        if kind == HeaderKind::Offset {
            output.push_str(
                "#[allow(dead_code)]\npub fn base_idx(name: &str) -> usize {\n    match name {\n",
            );
            for (name, value) in &defines {
                if let Some(register) = name.strip_suffix("_BASE_IDX")
                    && *value != 0
                    && defines.contains_key(register)
                {
                    let _ = writeln!(output, "        \"{register}\" => {value},");
                }
            }
            output.push_str("        _ => 0,\n    }\n}\n");
        }
        output
    }

    fn emit_arrays(header: &CHeader) -> String {
        let arrays = header.arrays();
        let capacity = arrays
            .values()
            .map(|values| values.len() * 12)
            .sum::<usize>()
            + 128;
        let mut output = Self::preamble_with_capacity(&header.path, capacity);
        for (name, values) in arrays {
            let _ = writeln!(output, "pub const {name}: [u32; {}] = [", values.len());
            for chunk in values.chunks(8) {
                let values = chunk
                    .iter()
                    .map(u64::to_string)
                    .collect::<Vec<_>>()
                    .join(", ");
                let _ = writeln!(output, "    {values},");
            }
            output.push_str("];\n");
        }
        output
    }

    fn preamble(source: &Path) -> String {
        Self::preamble_with_capacity(source, 128)
    }

    fn preamble_with_capacity(source: &Path, capacity: usize) -> String {
        let mut output = String::with_capacity(capacity);
        let _ = writeln!(
            output,
            "// AUTO-GENERATED by astra build.rs from Linux {LINUX_VERSION}: {}\n// Do not edit.",
            source.display()
        );
        output
    }
}

fn main() -> BuildResult<()> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    Generator::new(&manifest)?.run()
}
