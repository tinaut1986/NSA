#pragma once

// Console to console: a file goes from one 3DS to another over the local
// network. Ports are fixed, both on the same Wi-Fi:
//   UDP 5001  the receiver answers a probe ("NSA?") with "NSA!" and its name.
//   TCP 5002  the sender connects and sends a transfer_hello; the receiver
//             asks its user, answers one byte (1 accepted, 0 declined) and
//             then the file follows as it is.
// The receiver saves to sdmc:/nsa/received/ and may install it right after.

// "New 3DS XL - Marta": the model and the user name, as other consoles see this one.
void transfer_local_name(char* out, size_t size);

// The Receive screen: waits for a sender and asks what to do with its file.
void transfer_receive_open();

// The Send screen: lists the consoles that are receiving and sends the file
// to the one chosen. `archive` must stay open while the screen is.
void transfer_send_file(FS_Archive archive, const char* path, const char* name, u64 size);

// A title read from the system and sent as a package (see exportcia.c): the
// receiver builds the CIA. `name` is the CIA's file name and `size` the
// package's. The screen owns `stream` from now on.
typedef struct export_cia_data_s export_cia_data;
void transfer_send_stream(export_cia_data* stream, const char* name, u64 size);
