# noteamountgen

## how 2 build the jawn

you need cmake and gcc, ninja is optional but recommended

windows only btw, should probably work on wine? i don't have as linux install anymore so can't test.

do uhhhh

```bash
mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
```

then the exe should exist at `build/noteamountgen_gui.exe`

<img width="586" height="543" alt="image" src="https://github.com/user-attachments/assets/631459e9-e99e-47de-96e2-4b8dabefba12" />

---

this uses [sightread](https://github.com/GenericMadScientist/SightRead) by gms to parse charts, modified version is included to remove the boost dependency since we don't need qbmidi support.
