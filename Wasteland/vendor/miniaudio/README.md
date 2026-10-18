# miniaudio vendor notice

Vendored upstream single-header miniaudio from `mackron/miniaudio`, revision
`9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (`master` at retrieval).

The upstream `LICENSE` is included verbatim. This project elects its MIT No
Attribution alternative for redistribution. The header is used unmodified; the
engine requests a 48 kHz float32 stereo callback, and miniaudio's documented
`ma_device` converter resamples/converts that stream to the native endpoint
format and sample rate as required.

Upstream: <https://github.com/mackron/miniaudio>
