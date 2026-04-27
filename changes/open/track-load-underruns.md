# Track-load buffer underruns

## Intent

Loading a track produces a small but consistent burst of buffer underruns. The audio task has to open the file, scan for an ID3v2 header, initialise the decoder, and prime the ring buffer in quick succession — sample submission to I2S can't keep up while that runs, so a handful of underruns register on every load. The user wants the load path reshaped so that going from one track to the next is audibly clean and the underrun counter stays at zero in normal operation.
