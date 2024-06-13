#include <kernel/keyboard.h>
#include <kernel/apic.h>
#include <kernel/event.h>
#include <kernel/idt.h>
#include <kernel/spinlock.h>
#include <kernel/x86_64.h>

#define DATA 0x60
#define CONTROL 0x64
#define RX 1
#define TX_BUSY 2
#define AUX 0x20
#define CAPACITY 64

static struct {
    spinlock_t lock;
    bool extended;
    bool left_shift;
    bool right_shift;
    bool caps_lock;
    unsigned first;
    unsigned count;
    uint8_t bytes[CAPACITY];
} keyboard = { .lock = SPINLOCK_INIT };

static const char keys[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
    [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b', [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1a] = '[', [0x1b] = ']', [0x1c] = '\r',
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2b] = '\\',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};
static const char shifted[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0a] = '(', [0x0b] = ')',
    [0x0c] = '_', [0x0d] = '+', [0x1a] = '{', [0x1b] = '}', [0x27] = ':',
    [0x28] = '"', [0x29] = '~', [0x2b] = '|', [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

static char decode(uint8_t scan) {
    if (scan == 0xe0 || scan == 0xe1) {
        keyboard.extended = true;
        return 0;
    }
    if (keyboard.extended) {
        keyboard.extended = false;
        return 0;
    }
    bool down = !(scan & 0x80);
    unsigned code = scan & 0x7f;
    if (code == 0x2a) keyboard.left_shift = down;
    if (code == 0x36) keyboard.right_shift = down;
    if (code == 0x3a && down) keyboard.caps_lock = !keyboard.caps_lock;
    if (!down) return 0;
    char ch = keys[code];
    bool shift = keyboard.left_shift || keyboard.right_shift;
    if (ch >= 'a' && ch <= 'z') {
        if (shift != keyboard.caps_lock) ch -= 'a' - 'A';
    } else if (shift && shifted[code]) {
        ch = shifted[code];
    }
    return ch;
}

static void keyboard_irq(interrupt_frame_t *frame) {
    (void)frame;
    uint64_t flags;
    spin_lock_irqsave(&keyboard.lock, &flags);
    bool received = false;
    for (unsigned budget = 0; budget < 32; budget++) {
        uint8_t status = inb(CONTROL);
        if (!(status & RX)) break;
        uint8_t scan = inb(DATA);
        if (status & AUX) continue;
        char ch = decode(scan);
        if (!ch || keyboard.count == CAPACITY) continue;
        keyboard.bytes[(keyboard.first + keyboard.count++) % CAPACITY] = (uint8_t)ch;
        received = true;
    }
    spin_unlock_irqrestore(&keyboard.lock, flags);
    if (received) readiness_notify();
}

static bool controller_ready(unsigned bit, bool set) {
    for (unsigned i = 0; i < 100000; i++) {
        if (!!(inb(CONTROL) & bit) == set) return true;
        cpu_pause();
    }
    return false;
}

void keyboard_init(void) {
    if (!controller_ready(TX_BUSY, false)) return;
    outb(CONTROL, 0xae);
    if (!controller_ready(TX_BUSY, false)) return;
    for (unsigned i = 0; i < 32 && (inb(CONTROL) & RX); i++) (void)inb(DATA);
    outb(CONTROL, 0x20);
    if (!controller_ready(RX, true)) return;
    uint8_t config = inb(DATA);
    register_interrupt_handler(VEC_KEYBOARD, keyboard_irq);
    if (!ioapic_route_irq(1, VEC_KEYBOARD, lapic_get_id()) || !controller_ready(TX_BUSY, false)) return;
    outb(CONTROL, 0x60);
    if (controller_ready(TX_BUSY, false)) outb(DATA, (config | 0x41) & ~0x10);
}

bool keyboard_ready(void) {
    uint64_t flags;
    spin_lock_irqsave(&keyboard.lock, &flags);
    bool ready = keyboard.count != 0;
    spin_unlock_irqrestore(&keyboard.lock, flags);
    return ready;
}

int keyboard_getc(void) {
    uint64_t flags;
    spin_lock_irqsave(&keyboard.lock, &flags);
    int ch = -1;
    if (keyboard.count) {
        ch = keyboard.bytes[keyboard.first];
        keyboard.first = (keyboard.first + 1) % CAPACITY;
        keyboard.count--;
    }
    spin_unlock_irqrestore(&keyboard.lock, flags);
    return ch;
}
