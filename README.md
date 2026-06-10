# c2pancake

Original version developed at https://github.com/zhewenshen/c2pancake. Enhanced version developed by Yankai Zhu.

## Installing pancake compiler

1. Head to https://cakeml.org/regression.cgi and find the latest successful job on master.
2. Copy the download link for the artefacts.
3. `mkdir build && cd build && wget -c <LINK> -O - | tar -xz`
    - e.g. `mkdir build && cd build && wget -c https://cakeml.org/regression/artefacts/3364/cake-x64-64.tar.gz -O - | tar -xz`

## Additional features
- Omit statements with no effect in pancake output
    - variables with volatile specifier are not affected by this
    - previously, they were outputted as is which caused pancake compile errors
- array accesses and dereference operations of arbitrary depth are now properly hoisted
    - previously, nested array accesses (e.g. `a[b[c[1]]]`) would only be properly processed 1 layer deep and the dereference operator was not supported at all
