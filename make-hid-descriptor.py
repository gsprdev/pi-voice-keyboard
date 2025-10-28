#!/usr/bin/env python3
"""
Generate USB HID Boot Keyboard Report Descriptor with comments.
Creates a binary descriptor file with full documentation.
"""

def write_descriptor_with_comments(filename):
    """Write the boot keyboard descriptor with embedded comments."""
    
    # The standard USB HID Boot Keyboard report descriptor
    descriptor_bytes = bytes([
        0x05, 0x01,        # Usage Page (Generic Desktop)
        0x09, 0x06,        # Usage (Keyboard)
        0xa1, 0x01,        # Collection (Application)
        
        # Modifier keys (8 bits)
        0x05, 0x07,        #   Usage Page (Key Codes)
        0x19, 0xe0,        #   Usage Minimum (Left Control)
        0x29, 0xe7,        #   Usage Maximum (Right GUI)
        0x15, 0x00,        #   Logical Minimum (0)
        0x25, 0x01,        #   Logical Maximum (1)
        0x75, 0x01,        #   Report Size (1)
        0x95, 0x08,        #   Report Count (8)
        0x81, 0x02,        #   Input (Data, Variable, Absolute)
        
        # Reserved byte
        0x95, 0x01,        #   Report Count (1)
        0x75, 0x08,        #   Report Size (8)
        0x81, 0x03,        #   Input (Constant)
        
        # LED indicators (5 bits)
        0x95, 0x05,        #   Report Count (5)
        0x75, 0x01,        #   Report Size (1)
        0x05, 0x08,        #   Usage Page (LEDs)
        0x19, 0x01,        #   Usage Minimum (Num Lock)
        0x29, 0x05,        #   Usage Maximum (Kana)
        0x91, 0x02,        #   Output (Data, Variable, Absolute)
        
        # LED padding (3 bits)
        0x95, 0x01,        #   Report Count (1)
        0x75, 0x03,        #   Report Size (3)
        0x91, 0x03,        #   Output (Constant)
        
        # Key codes (6 bytes, 48 keys max)
        0x95, 0x06,        #   Report Count (6)
        0x75, 0x08,        #   Report Size (8)
        0x15, 0x00,        #   Logical Minimum (0)
        0x25, 0x65,        #   Logical Maximum (101)
        0x05, 0x07,        #   Usage Page (Key Codes)
        0x19, 0x00,        #   Usage Minimum (0)
        0x29, 0x65,        #   Usage Maximum (101)
        0x81, 0x00,        #   Input (Data, Array)
        
        0xc0               # End Collection
    ])
    
    # Write binary descriptor
    with open(filename, 'wb') as f:
        f.write(descriptor_bytes)
    
    print(f"✓ Binary descriptor written to {filename}")
    print(f"  Size: {len(descriptor_bytes)} bytes")
    
    # Also create a commented version for reference
    comment_filename = filename + ".txt"
    with open(comment_filename, 'w') as f:
        f.write("""USB HID Boot Keyboard Report Descriptor
===============================================

This is the standard USB HID Boot Keyboard descriptor that provides
maximum compatibility with BIOS, bootloaders, and all operating systems.

Report Format (8 bytes):
-----------------------
Byte 0: Modifier keys (bitfield)
  Bit 0: Left Control
  Bit 1: Left Shift  
  Bit 2: Left Alt
  Bit 3: Left GUI (Windows/Command)
  Bit 4: Right Control
  Bit 5: Right Shift
  Bit 6: Right Alt
  Bit 7: Right GUI

Byte 1: Reserved (always 0)

Bytes 2-7: Key codes (up to 6 simultaneous keys)
  Each byte is a HID usage code for a pressed key
  0x00 = no key, 0x04 = 'a', 0x05 = 'b', etc.

LED Support:
-----------
The descriptor includes LED indicators that the host can control:
- Num Lock, Caps Lock, Scroll Lock, Compose, Kana

Usage Pages:
-----------
- 0x01: Generic Desktop (keyboard device)
- 0x07: Key Codes (actual key values)  
- 0x08: LEDs (indicator lights)

This descriptor is compatible with:
- All major operating systems
- BIOS and bootloader environments
- Virtual machines and containers
- Embedded systems

""")
        
        # Add hex dump with comments
        f.write("\nHex Dump with Comments:\n")
        f.write("=" * 50 + "\n")
        
        lines = [
            ("05 01", "Usage Page (Generic Desktop)"),
            ("09 06", "Usage (Keyboard)"),
            ("a1 01", "Collection (Application)"),
            ("", ""),
            ("05 07", "Usage Page (Key Codes)"),
            ("19 e0", "Usage Minimum (Left Control)"),
            ("29 e7", "Usage Maximum (Right GUI)"),
            ("15 00", "Logical Minimum (0)"),
            ("25 01", "Logical Maximum (1)"),
            ("75 01", "Report Size (1)"),
            ("95 08", "Report Count (8) - Modifier keys"),
            ("81 02", "Input (Data, Variable, Absolute)"),
            ("", ""),
            ("95 01", "Report Count (1)"),
            ("75 08", "Report Size (8)"),
            ("81 03", "Input (Constant) - Reserved byte"),
            ("", ""),
            ("95 05", "Report Count (5)"),
            ("75 01", "Report Size (1)"),
            ("05 08", "Usage Page (LEDs)"),
            ("19 01", "Usage Minimum (Num Lock)"),
            ("29 05", "Usage Maximum (Kana)"),
            ("91 02", "Output (Data, Variable, Absolute) - LED states"),
            ("", ""),
            ("95 01", "Report Count (1)"),
            ("75 03", "Report Size (3)"),
            ("91 03", "Output (Constant) - LED padding"),
            ("", ""),
            ("95 06", "Report Count (6) - Key codes"),
            ("75 08", "Report Size (8)"),
            ("15 00", "Logical Minimum (0)"),
            ("25 65", "Logical Maximum (101)"),
            ("05 07", "Usage Page (Key Codes)"),
            ("19 00", "Usage Minimum (0)"),
            ("29 65", "Usage Maximum (101)"),
            ("81 00", "Input (Data, Array) - Key codes"),
            ("", ""),
            ("c0", "End Collection"),
        ]
        
        for hex_bytes, comment in lines:
            if hex_bytes:
                f.write(f"{hex_bytes:<8} # {comment}\n")
            else:
                f.write("\n")
    
    print(f"✓ Commented reference written to {comment_filename}")

if __name__ == "__main__":
    import sys
    filename = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hid-descriptor.bin"
    write_descriptor_with_comments(filename)
  