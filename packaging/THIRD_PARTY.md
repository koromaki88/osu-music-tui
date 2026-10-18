# Bundled dependencies

This local AppImage bundles the C++ player, the Realm .NET adapter, .NET 8, and
native libraries collected from the build machine. glibc and the dynamic loader
are deliberately not bundled. Linux audio services/drivers remain host-provided.

The adjacent `dotnet-and-nuget/` directory contains available package license
notices and NuGet metadata. Native package license/copyright files are collected
from the build system; common license texts are included where available.
`../osu-player/build-manifest.json` records native library hashes, package versions,
the .NET runtime version, and the measured minimum glibc version.

**This is not a redistribution compliance certificate.** System FFmpeg builds
can enable GPL libraries (and may have additional codec/patent restrictions).
Before publishing binaries, review the manifest and all licenses, supply required
corresponding source/build materials and notices, and decide the project's own
license. Collecting license texts alone does not satisfy every redistribution
obligation. No application license is selected automatically by this builder.

.NET: https://github.com/dotnet/runtime
Realm .NET: https://github.com/realm/realm-dotnet
Realm Core: https://github.com/realm/realm-core
FFmpeg: https://ffmpeg.org/legal.html
AppImage tooling: https://github.com/AppImage/appimagetool
AppImage runtime: https://github.com/AppImage/type2-runtime
