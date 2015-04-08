# denon-multiplexd

## Motivation

A simple daemon to multiplex control connections to a Denon networked A/V receiver. By default they seem to only support one network connection at a time. This allows multiple connections.

Each client connection will see all output from the receiver. Any commands from client connections will be forwarded to the receiver in the order received.

## Features

 * Translates '\r' in the Denon protocol to '\n' in client connections.
 * Automatically rate-limits commands sent to the receiver.

## Testing

I have only tested this on my AVR-3311CI.

## Usage

To run the daemon:

    $ denon-multiplexd <receiver ip address>

For easy prototyping, netcat can be used to manually interact with the receiver.

    $ netcat 127.0.0.1 33893
    PWON
    MV50
    ...
 
