# Third-party notices

The MIT license in `LICENSE` covers this project's own code. The following are
included in, or used to build, this repository under their own terms.

## Spleen (font)

`tools/spleen-5x8.bdf` is the Spleen 5x8 bitmap font, and the generated
headers `dsclient/source/font.h`, `font4x6.h` and `font5x7.h` are derived from
it by `tools/genfont.py`; `dsterm/dsfont.py`, which the server package ships for
`dsterm view`, is copied from those headers by `tools/genviewfont.py`.

```
Copyright (c) 2018-2026, Frederic Cambus
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## Fusion Pixel Font (Chinese and Japanese)

`tools/fusion-pixel-10px-subset.bdf` is a subset of Fusion Pixel Font, 10px
Monospaced, cut to the characters the DS's built-in Chinese and Japanese
strings use; the CJK glyphs in the generated `dsclient/source/ui_font.h` are
derived from it by `tools/genuifont.py`. Fusion Pixel's 10px glyphs come from
Ark Pixel Font, with BoutiqueBitmap9x9 filling gaps; all three are under the
SIL Open Font License 1.1, and none reserves a font name.

- Fusion Pixel Font, https://github.com/TakWolf/fusion-pixel-font,
  Copyright (c) 2022, TakWolf (https://takwolf.com).
- Ark Pixel Font, https://github.com/TakWolf/ark-pixel-font,
  Copyright (c) 2021, TakWolf (https://takwolf.com).
- BoutiqueBitmap9x9, https://github.com/scott0107000/BoutiqueBitmap9x9,
  Copyright (c) scott0107000.

```
-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## BlocksDS build template

`dsclient/Makefile` and `dsclient/Makefile.arm7` are based on the BlocksDS
example Makefiles by Antonio Niño Díaz, released under CC0-1.0 (public domain
dedication); their SPDX headers are kept. The ROM links against BlocksDS's
libnds, dswifi, Maxmod and libc, which are distributed under their own
(permissive) licenses as part of the toolchain and are not vendored here.

## BlocksDS default ARM7 core

`dsclient/arm7/source/main.c` is modified from the default ARM7 core of
BlocksDS (`sys/arm7/main_core`), Zlib license: Copyright (C) 2005 Michael
Noland (joat), Copyright (C) 2005 Jason Rogers (Dovoto), Copyright (C)
2005-2015 Dave Murphy (WinterMute), Copyright (C) 2023 Antonio Niño Díaz. The
modification (Wi-Fi updates guarded against nesting) is marked in the file.

```
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
```

## Python dependencies (installed by pip, not vendored)

- [pyte](https://github.com/selectel/pyte): LGPL-3.0
- [wcwidth](https://github.com/jquast/wcwidth): MIT

## Speech-to-text (optional, installed by you, not distributed)

Push-to-talk can use a Whisper backend that you install yourself:
[mlx-whisper](https://github.com/ml-explore/mlx-examples),
[faster-whisper](https://github.com/SYSTRAN/faster-whisper),
[openai-whisper](https://github.com/openai/whisper) or
[whisper.cpp](https://github.com/ggml-org/whisper.cpp)'s `whisper-cli`, and
for some of them [ffmpeg](https://ffmpeg.org). None of them is a dependency of
this project or included in its downloads; each comes under its own license.
The model weights they download on first use come under their publishers'
own terms.

## Sounds

The sound set in `dsclient/sounds/` was generated with
[ElevenLabs](https://elevenlabs.io) sound-effect generation for this project,
then cut, re-pitched and arranged for the DS (see `tools/sfx_build.py`). It is
distributed with the project under its MIT license.
