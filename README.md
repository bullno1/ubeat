# ubeat

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

# What?

A [bytebeat](http://canonical.org/~kragen/bytebeat/) live coding environment with uxntal as the language?

# Why?

As I was experimenting with bytebeat live coding using [my own implementation](https://github.com/bullno1/sbeat/), I found the expression language quite limited.
While it can be extended, I feel like I should use a fully featured programming language instead.

# How?

To build, run: `make`.

Then run: `./ubeat sample.tal`.
It will play the classic "42" tune (`t*(42&t>>10)`).
The file can be edited and the tune will be updated immediately.
A compile error will not update the music.

The sample is also interactive:

* Holding left mouse will play the tune backward
* Middle mouse will pause
* Right mouse reset `t` to 0
* Mouse wheel can adjust the constant factor in the classic 42 tune.
  The blue line is a reference for the value 42.
  The red line is the current constant factor.

# Bytebeat device specification

This is a work in progress.

The specification can be found in the [sample](sample.tal).

To ensure low latency, the vector will be evaluated in a separate thread in its own VM.

## Communication with the main thread

Communication can be achieved through the Bytebeat device or the zero page.

Whenever the main thread writes to the following device ports, the value will be synchronized:

* `Bytebeat/vector`: This allows switching the current tune.
* `Bytebeat/t`: This allows resetting or seeking.
* `Bytebeat/v`: This allows pausing, [playing backward](https://en.wikipedia.org/wiki/Backmasking) or speeding up.
  Writing `ffff` to this port will make the tune run backward.

Whenever the main thread writes to the zero-page, the content will be synchronized with the audio thread.
The bytebeat vector will be able to read from it.

Take note that since the audio thread works on its own schedule, all communications are asynchronous.
That is, do not expect the audio thread to response immediately to commands.
