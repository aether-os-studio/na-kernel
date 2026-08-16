use alloc::vec::Vec;
use core::{ops::Range, time::Duration};

use na_std::{
    Error, KernelLog, Result,
    net::{self, MacAddress},
    sync::Mutex,
    time,
    usb::{self, ControlData, ControlRequest, Direction, Recipient, RequestKind, TransferType},
};

use crate::protocol::{self, Oid, Packet, Request, WireMessage};

const USB_CLASS_CDC_DATA: u8 = 0x0a;
const SEND_ENCAPSULATED_COMMAND: u8 = 0x00;
const GET_ENCAPSULATED_RESPONSE: u8 = 0x01;
const HOST_MAX_TRANSFER_SIZE: usize = 64 * 1024;
const CONTROL_RESPONSE_SIZE: usize = 1024;
const RESPONSE_ATTEMPTS: usize = 20;
const RESPONSE_RETRY_DELAY: Duration = Duration::from_millis(10);
const PACKET_FILTER: u32 = 0x0000_0001 | 0x0000_0004 | 0x0000_0008;

pub struct RndisDevice {
    transmitter: Mutex<Transmitter>,
    receiver: Mutex<Receiver>,
}

impl RndisDevice {
    pub fn bind(
        device: usb::Device,
        control_interface: usb::Interface,
    ) -> Result<net::Registration<Self>> {
        let control_info = control_interface.info()?;
        let pipes = DataPipes::open(device)?;
        let mut control = ControlChannel::new(device, control_info.number);
        let device_max_transfer = control.initialize(HOST_MAX_TRANSFER_SIZE as u32)?;
        let mac = control.mac_address()?;
        let mtu = control.query_u32(Oid::MaximumFrameSize)?;
        let transfer_size = device_max_transfer.min(HOST_MAX_TRANSFER_SIZE);
        let required = protocol::PACKET_HEADER_SIZE
            .checked_add(usize::try_from(mtu).map_err(|_| Error::Range)?)
            .and_then(|size| size.checked_add(net::ETHERNET_FRAME_OVERHEAD))
            .ok_or(Error::Range)?;
        if required > transfer_size {
            return Err(Error::NoDevice);
        }
        control.set_u32(Oid::PacketFilter, PACKET_FILTER)?;

        let adapter = Self::new(pipes, transfer_size)?;
        let registration = net::DeviceBuilder::new(adapter, mac, mtu).register()?;
        KernelLog::write_fmt(format_args!(
            "rndis: registered {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}, mtu {}\n",
            mac.bytes()[0],
            mac.bytes()[1],
            mac.bytes()[2],
            mac.bytes()[3],
            mac.bytes()[4],
            mac.bytes()[5],
            mtu
        ));
        Ok(registration)
    }

    fn new(pipes: DataPipes, transfer_size: usize) -> Result<Self> {
        Ok(Self {
            transmitter: Mutex::new(Transmitter::new(pipes.output))?,
            receiver: Mutex::new(Receiver::new(pipes.input, transfer_size)?)?,
        })
    }
}

impl net::Device for RndisDevice {
    fn transmit(&self, frame: &[u8]) -> Result<()> {
        self.transmitter.lock().transmit(frame)
    }

    fn receive(&self, frame: &mut [u8]) -> Result<usize> {
        self.receiver.lock().receive(frame)
    }
}

struct DataPipes {
    input: usb::Pipe,
    output: usb::Pipe,
}

impl DataPipes {
    fn open(device: usb::Device) -> Result<Self> {
        device
            .interfaces()
            .filter(|interface| {
                interface
                    .info()
                    .is_ok_and(|info| info.class_code == USB_CLASS_CDC_DATA)
            })
            .chain(device.interfaces().filter(|interface| {
                interface
                    .info()
                    .is_ok_and(|info| info.class_code != USB_CLASS_CDC_DATA)
            }))
            .find_map(|interface| Self::open_interface(interface).ok())
            .ok_or(Error::NoDevice)
    }

    fn open_interface(interface: usb::Interface) -> Result<Self> {
        Ok(Self {
            input: interface.open_pipe(TransferType::Bulk, Direction::In)?,
            output: interface.open_pipe(TransferType::Bulk, Direction::Out)?,
        })
    }
}

struct ControlChannel {
    device: usb::Device,
    interface_number: u8,
    next_request_id: u32,
    response: [u8; CONTROL_RESPONSE_SIZE],
}

impl ControlChannel {
    const fn new(device: usb::Device, interface_number: u8) -> Self {
        Self {
            device,
            interface_number,
            next_request_id: 1,
            response: [0; CONTROL_RESPONSE_SIZE],
        }
    }

    fn initialize(&mut self, max_transfer_size: u32) -> Result<usize> {
        self.execute(Request::Initialize { max_transfer_size })?
            .initialize_max_transfer_size()
    }

    fn query_u32(&mut self, oid: Oid) -> Result<u32> {
        let response = self.execute(Request::Query { oid })?;
        WireMessage::read_u32(response.query_data()?)
    }

    fn mac_address(&mut self) -> Result<MacAddress> {
        self.query_mac(Oid::CurrentAddress)
            .or_else(|_| self.query_mac(Oid::PermanentAddress))
    }

    fn query_mac(&mut self, oid: Oid) -> Result<MacAddress> {
        let response = self.execute(Request::Query { oid })?;
        let bytes: [u8; 6] = response
            .query_data()?
            .get(..6)
            .ok_or(Error::Io)?
            .try_into()
            .unwrap();
        let address = MacAddress::new(bytes);
        address.is_unicast().then_some(address).ok_or(Error::Io)
    }

    fn set_u32(&mut self, oid: Oid, value: u32) -> Result<()> {
        self.execute(Request::SetU32 { oid, value }).map(|_| ())
    }

    fn execute(&mut self, request: Request) -> Result<WireMessage<'_>> {
        let command = request.encode(self.take_request_id());
        let sent = self.device.control(
            self.control_request(Direction::Out, SEND_ENCAPSULATED_COMMAND),
            ControlData::Out(command.bytes()),
        )?;
        if sent != command.bytes().len() {
            return Err(Error::Io);
        }

        for _ in 0..RESPONSE_ATTEMPTS {
            let request = self.control_request(Direction::In, GET_ENCAPSULATED_RESPONSE);
            let received = match self
                .device
                .control(request, ControlData::In(&mut self.response))
            {
                Ok(received) => received,
                Err(Error::NoDevice) => return Err(Error::NoDevice),
                Err(_) => {
                    time::delay(RESPONSE_RETRY_DELAY);
                    continue;
                }
            };
            if received != 0 {
                let response = WireMessage::parse(&self.response[..received])?;
                if response.kind() == protocol::MESSAGE_INDICATE_STATUS {
                    continue;
                }
                response.validate_completion(&command)?;
                return Ok(response);
            }
            time::delay(RESPONSE_RETRY_DELAY);
        }
        Err(Error::Io)
    }

    fn control_request(&self, direction: Direction, request: u8) -> ControlRequest {
        ControlRequest::new(
            direction,
            RequestKind::Class,
            Recipient::Interface,
            request,
            0,
            self.interface_number.into(),
        )
    }

    fn take_request_id(&mut self) -> u32 {
        let request_id = self.next_request_id;
        self.next_request_id = self.next_request_id.wrapping_add(1);
        if self.next_request_id == 0 {
            self.next_request_id = 1;
        }
        request_id
    }
}

struct Transmitter {
    pipe: usb::Pipe,
    buffer: Vec<u8>,
}

impl Transmitter {
    const fn new(pipe: usb::Pipe) -> Self {
        Self {
            pipe,
            buffer: Vec::new(),
        }
    }

    fn transmit(&mut self, frame: &[u8]) -> Result<()> {
        Packet::new(frame).write(&mut self.buffer)?;
        let written = self.pipe.write(&self.buffer)?;
        (written == self.buffer.len())
            .then_some(())
            .ok_or(Error::Io)
    }
}

struct Receiver {
    pipe: usb::Pipe,
    buffer: Vec<u8>,
    cursor: usize,
    used: usize,
}

impl Receiver {
    fn new(pipe: usb::Pipe, transfer_size: usize) -> Result<Self> {
        let mut buffer = Vec::new();
        buffer
            .try_reserve_exact(transfer_size)
            .map_err(|_| Error::OutOfMemory)?;
        buffer.resize(transfer_size, 0);
        Ok(Self {
            pipe,
            buffer,
            cursor: 0,
            used: 0,
        })
    }

    fn receive(&mut self, frame: &mut [u8]) -> Result<usize> {
        loop {
            if let Some(range) = self.next_packet()? {
                let packet = &self.buffer[range];
                if packet.len() > frame.len() {
                    return Err(Error::Range);
                }
                frame[..packet.len()].copy_from_slice(packet);
                return Ok(packet.len());
            }
            self.cursor = 0;
            self.used = self.pipe.read(&mut self.buffer)?;
            if self.used == 0 {
                return Ok(0);
            }
        }
    }

    fn next_packet(&mut self) -> Result<Option<Range<usize>>> {
        while self.cursor < self.used {
            let start = self.cursor;
            let message = match WireMessage::parse(&self.buffer[start..self.used]) {
                Ok(message) => message,
                Err(error) => {
                    self.cursor = self.used;
                    return Err(error);
                }
            };
            self.cursor = self
                .cursor
                .checked_add(message.length())
                .ok_or(Error::Range)?;
            if let Some(range) = message.packet_range()? {
                return Ok(Some(start + range.start..start + range.end));
            }
        }
        Ok(None)
    }
}
