#pragma once

struct tty_session;
struct tty_graphics_;

int terminal_rebind_framebuffer(struct tty_session *tty,
                                const struct tty_graphics_ *framebuffer);
