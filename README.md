```
$ mkdir build && cd build && cmake .. && make
$ ./fmwav
Usage: ./fmwav <frequency> [length]

Parameters:
  frequency    (MHz)  - Required. The frequency to tune into.
  length       (sec)  - Optional. The length of the recording. Defaults to 30 seconds.

Description:
  This utility uses an RTL-SDR device to listen on the given frequency,
  FM demodulate the signal, and store a WAV file containing the recording.
```
