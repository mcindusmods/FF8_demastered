bx and bimg, from source (the Release build only)
=================================================

vendor\lib\bgfx has bx and bimg (the libraries DEMASTER reads and writes images with) as Debug builds only. A Debug
library can't be linked into a release build (different C++ runtime), so the Release configuration of ff8_demaster compiles
them from here instead. The Debug configurations don't use this folder: they link vendor\lib\bgfx as before.

These are the revisions the headers in vendor\include\bx and vendor\include\bimg come from (every header there is identical
to the one in these revisions), unchanged:

  bx    https://github.com/bkaradzic/bx    commit 0455026c42c9ed47d44c098a939bb7001a3ff631 (2020-05-13)
        src\ (built as one file: src\amalgamated.cpp), 3rdparty\ini
  bimg  https://github.com/bkaradzic/bimg  commit 5ee42f6 (2020-04-19)
        src\image.cpp, image_gnf.cpp, image_decode.cpp (bimg and bimg_decode; bimg_encode isn't used by DEMASTER)
        3rdparty\astc-codec (the decoder only), lodepng, stb (stb_image.h), tinyexr

Licences: bx and bimg BSD 2-clause (bx\LICENSE, bimg\LICENSE); astc-codec Apache 2.0 (its LICENSE); lodepng zlib; stb_image
public domain / MIT; tinyexr BSD 3-clause (each stated at the top of its file).

To update: replace the headers in vendor\include and these sources from the same pair of revisions, together.
