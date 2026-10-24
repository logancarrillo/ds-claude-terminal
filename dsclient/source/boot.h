// From power-on to a server: the boot menu, the Wi-Fi wizard, discovery and
// pairing.
//
// A card with nothing on it but dsterm.nds boots into this, and is connected
// within a minute: join a network, find the server (or type its address),
// compare the code `dsterm pair` shows, and the terminal appears.  A returning
// card counts down from three on its last server and connects with no input,
// which is the old behaviour.  An old card with /dsterm.cfg is migrated into
// /dsterm/ on its first boot and carries on as before.
//
// libnds and dswifi glue.  Everything that can be pure is: bootui.h (fields,
// lists, labels), profiles.h (the card's files), discover.h (datagrams and
// the sweep), pair.h and x25519.h (the exchange), entropy.h (the key's
// randomness).
#ifndef DSTERM_BOOT_H
#define DSTERM_BOOT_H

#include "config.h"
#include "console.h"
#include "remote.h"

// Run the menu until the person (or the countdown) picks a server, then make
// sure Wi-Fi is associated for it.  Fills `cfg` and returns 1.  `notice` is
// shown at the top of the menu and stops the countdown -- main() passes why
// the last connection failed, so a bad server does not auto-retry forever
// with the card in a loop nobody can break.
int boot_run(dsterm_config_t *cfg, const char *notice);

// This connection's card nonce (PROTO_CARD_NONCE_BYTES), drawn from the
// boot menu's entropy pool after stirring in the clock, the frame count, the
// scanline and how many connections this boot has made.  It need not be
// secret, only never repeated: the count separates two connections in one
// boot, and the clock and the timing of everything before it separate boots.
void boot_connect_nonce(uint8_t *out);
// Settings' AUTOCONNECT.  On (the default), the menu counts down from three
// and connects to the last computer by itself; off, it waits on that row for
// a press.
void boot_set_autoconnect(int on);

// REMOTE's other computers.  open/close bracket the picker being on screen;
// poll once a frame while it is, to probe now and then and collect the
// answers.  list() merges them with the saved servers, marking the one at
// host:port as current.
void boot_peers_open(void);
void boot_peers_poll(void);
void boot_peers_close(void);
void boot_peers_list(remote_computers_t *out, const char *host, uint16_t port);
// "DSi (DSi mode)", "DS Lite" ... (console.h), for the log and ABOUT.
const char *boot_console_name(void);
// Which console this is (console.h): the controls guide draws it.
console_family_t boot_console_family(void);
// The saved name of the server last connected, for "CLAUDE ON <name>".
const char *boot_server_name(void);
// Ready `cfg` to connect to `m`, pairing first (on the boot menu's screens)
// if this card has never met it.  1 when `cfg` is ready; 0 if the person
// backed out or pairing failed.
int boot_switch_to(const remote_computer_t *m, dsterm_config_t *cfg);
// The server just authenticated: after a switch, make it the one the card
// connects to at the next power-on; and if it was reached at a new address
// (a moved computer), save that address.  Until this is called, a failed
// attempt leaves everything saved exactly as it was.
void boot_switch_done(void);
// The last computer did not answer where it is saved: ask the network whether
// it answers somewhere else under the same name.  On finding it, readies
// `cfg` for the new address and returns 1; the address is saved once the
// connection there has authenticated (boot_switch_done()).
int boot_find_moved_last(dsterm_config_t *cfg);

#endif  // DSTERM_BOOT_H
