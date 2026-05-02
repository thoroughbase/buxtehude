# buxtehude

Buxtehude is an interprocess communication [protocol](/docs/spec.md) and library
implementing the protocol of the same name.
It currently supports message transmission over TCP and UNIX sockets and within a single
process using a homogeneous API.
Messages transmitted over sockets are encoded in either MessagePack or JSON.

## Dependencies

- [libevent 2.1.12-stable](https://libevent.org/) - Must be built with thread support
- [json (nlohmann)](https://github.com/nlohmann/json)
- [tb-cpp (dna65)](https://github.com/thoroughbase/tb-cpp)

## Usage

### Basics

A call to `buxtehude::Initialise()` must be made before using any other part of the
library.

Applications connect to and communicate with each other over a buxtehude Server
as Clients. All clients are part of a team with a name decided upon joining. Clients
can then send messages to all clients under a team name, or to only the first
"available" client under that name.

In this implementation, Servers and Clients run in their own separate threads, meaning
an application may concurrently have many of each.

### Creating a server

The following code snippet launches a server listening on internet port 6666.

```cpp
bux::Server server;

if (auto err = server.IPServer(6666); err.is_error()) {
    std::cout << "Error starting server: " << err.get_error().What() << '\n';
    // Error handling
}
```

The API does not expose any means for communicating through the Server object.
An application that wishes to have the server "respond" to clients must do so through
another buxtehude Client - an "internal" client is well suited to this purpose.

### Creating a client

The following code snippet creates a client that will join the team "client", and
connect to the local server listening on the port 6666. The ClientPreferences also
allow specification of what format messages should be encoded in. This implementation
supports all the same formats as the technical specification, so there is no difference
from the programmer's perspective when using this library.

```cpp
bux::Client client(bux::ClientPreferences {
    .teamname = "client"
});

if (auto err = client.IPConnect("127.0.0.1", 6666); err.is_error()) {
    std::cout << "Couldn't connect to server: " << err.get_error().What() << '\n';
    // Error handling
}
```

Clients can send Messages, which have fields described in the
[specification](/docs/spec.md), to a particular destination, which may be every client
(`bux::MSG_ALL`), or a team.

```cpp
client.Write(bux::Message {
    .dest { bux::MSG_ALL }, // If omitted, only sends to the server (not to any clients)
    .type = "message", // Mandatory
    .content = "hello world", // May be omitted
    .only_first = false // Defaults to false
}).if_err([] (bux::WriteError) { // Returns a `WriteError` if the connection has closed.
    std::cout << "Error sending message\n";
});
```

Clients can define behaviour upon receipt of messages of a particular type by adding
a handler.

```cpp
client.AddHandler("type", [] (bux::Client& self, const bux::Message& message) {
    // ...
})
```
