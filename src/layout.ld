/* Near memory, reserved by the linker rather than placed by hand.

   Everything here is NOLOAD: the space is claimed, not filled, so reserving it
   costs no image bytes. What it buys is that an overflow is a link error naming
   the region, rather than two tenants quietly sharing bytes.

   ram_low is $0300-$1FFF and this program uses it for nothing else, so the
   animation VM's 3,787 bytes of sprite, timer and rendezvous tables go there
   whole. The item database and the rest of the VM state go to ram_fixed, which
   is also where code lives -- the one region under real pressure once the
   interpreter is resident, which is why the biggest tenant is kept out of it.

   KEEP, because nothing references these until the loader does and the
   collector would otherwise drop the very reservation being made -- silently,
   leaving a link that looks right and a region that was never claimed. */
SECTIONS {
  .vmstate (NOLOAD) : { KEEP(*(.vmstate .vmstate.*)) } >ram_low
  .gamedb  (NOLOAD) : { KEEP(*(.gamedb .gamedb.*)) }   >ram_fixed
} INSERT AFTER .bank_0;
