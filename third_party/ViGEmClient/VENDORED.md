Vendored from https://github.com/nefarius/ViGEmClient at commit b66d02d57e32cc8595369c53418b843e958649b4

Nested .git removed so this is a plain vendored copy rather than an embedded
repository -- git otherwise warns that clones of the outer repo get an empty
directory here.

Built from source by the top-level CMakeLists (one .cpp) rather than consuming a
prebuilt lib, so it matches our CRT settings.

Requires the ViGEmBus kernel driver at runtime:
  https://github.com/nefarius/ViGEmBus/releases  (v1.22.0, archived Nov 2023)
  installer SHA256 89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A
  signed by "Nefarius Software Solutions e.U.", DigiCert Trusted G4
