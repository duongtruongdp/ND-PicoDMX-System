ND DMX NODE 4U v1.0.0
======================

Recommended Arduino-Pico settings for a standard Raspberry Pi Pico:
- Board: Raspberry Pi Pico
- CPU Speed: 133 MHz
- Flash Size: 2MB (no filesystem required)
- USB Stack: Pico SDK
- Optimize: Small (-Os)

Network defaults:
- Node IP: 10.10.10.10
- Subnet: 255.255.255.0
- Gateway/DNS: 10.10.10.1
- Dashboard: http://10.10.10.10/
- Status API: http://10.10.10.10/api/status

Dashboard architecture:
- One static HTML/CSS/JS document stored in flash.
- No Google Fonts, CDN, images, frameworks or external assets.
- Impact headlines, Helvetica Neue body text, Consolas technical values.
- HTML is transmitted with Content-Length and retry-safe 512-byte writes.
- Art-Net and sACN are serviced between web transfer blocks.
- Status refresh is one small JSON request per second.

After upload:
1. Power-cycle the node.
2. Put the computer on 10.10.10.x / 255.255.255.0.
3. Open http://10.10.10.10/.
4. Perform one hard refresh to clear the previous broken dashboard from cache.

Validation completed before delivery:
- C++ syntax checked using API stubs.
- Embedded JavaScript passed Node.js syntax checking.
- HTML parsed successfully.

The firmware has not been compiled with your exact installed Arduino-Pico and
Ethernet library versions or tested on your physical node. Test all four DMX
ports and the local output test controls before production use.
