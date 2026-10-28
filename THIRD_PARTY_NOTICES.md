# Third-party notices

Metrotonopitecus is released under the [MIT License](LICENSE). The ToneMap view
and the pitch detector it uses are ports of the open-source projects below. Their
licenses apply to them, not to the rest of this project, and their copyright
notices must be kept.

The headers of the ported source files (`src/tonemap_*`, `src/pitch_mpm.*`,
`test/tonemap_vectors.h`, `tools/gen_tonemap_vectors.py`) say which parts come
from these projects.

## 1. Tonemap.Live

- Source: <https://github.com/crossrhythm/tonemap.live>
- Website: <https://tonemap.live>
- Used for: the ToneMap heat-map model and its view, ported from JavaScript and
  HTML to C++ for the ESP32-S3.
- License: MIT

```
MIT License

Copyright (c) 2025 Jeremy Black

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 2. pitchy 4.0.1

- Source: <https://github.com/ianprime0509/pitchy> (version 4.0.1)
- Used for: the pitch detector (McLeod pitch method), ported from TypeScript
  to C++.
- License: MIT. Later versions of pitchy changed their license; the text below
  is the one that comes with version 4.0.1.

```
Copyright 2018-2021 Ian Johnson

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
