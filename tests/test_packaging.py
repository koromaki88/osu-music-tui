"""Fast packaging safety checks; no downloads, .NET, containers, or real library."""

import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_ca_bundle as trust

spec = importlib.util.spec_from_file_location(
    "package_appimage", Path(__file__).resolve().parent.parent / "tools/package_appimage.py")
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)
builder_spec = importlib.util.spec_from_file_location(
    "build_appimage", Path(__file__).resolve().parent.parent / "tools/build_appimage.py")
builder = importlib.util.module_from_spec(builder_spec)
builder_spec.loader.exec_module(builder)


class PackagingTests(unittest.TestCase):
    def test_ca_bundle_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "bundle.pem"
            key = Path(directory) / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", str(key), "-out", str(bundle), "-days", "1",
                            "-subj", "/CN=Synthetic packaging root"], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            import ssl
            digest = hashlib.sha256(ssl.PEM_cert_to_DER_cert(bundle.read_text())).hexdigest()
            self.assertEqual(trust.check_ca_bundle(bundle), {"policy": trust.POLICY, "certificates": 1})
            with patch.dict(trust.BLOCKED_ANCHORS, {digest: "Synthetic blocked root"}):
                with self.assertRaisesRegex(RuntimeError, "Synthetic blocked root"):
                    trust.check_ca_bundle(bundle)
            for content in ("", "not a PEM certificate", "-----BEGIN CERTIFICATE-----\nbm90LWEtY2VydA==\n-----END CERTIFICATE-----\n"):
                bundle.write_text(content)
                with self.assertRaises(RuntimeError):
                    trust.check_ca_bundle(bundle)

    def parse(self, output, returncode=0):
        completed = subprocess.CompletedProcess([], returncode, stdout=output, stderr="")
        with patch.object(packaging.subprocess, "run", return_value=completed):
            return packaging.dependencies(Path("/trusted/player"))

    def test_native_libraries_exclude_glibc_and_absolute_loader(self):
        result = self.parse("""\
        libavcodec.so.59 => /usr/lib/libavcodec.so.59 (0x123)
        libc.so.6 => /usr/lib/libc.so.6 (0x456)
        /lib64/ld-linux-x86-64.so.2 => /usr/lib/ld-linux-x86-64.so.2 (0x789)
        """)
        self.assertEqual(set(result), {"libavcodec.so.59"})
        self.assertTrue(all("/" not in name for name in result))

    def test_missing_or_path_based_dependencies_are_fatal(self):
        for output in ("libfoo.so.1 => not found", "/some/libfoo.so => /usr/lib/libfoo.so (0x123)"):
            with self.assertRaises(RuntimeError):
                self.parse(output)

    def test_sanitizer_build_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "sanitizer"):
            self.parse("libasan.so.8 => /usr/lib/libasan.so.8 (0x123)")

    def test_cached_tool_must_match_pinned_checksum(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / ".tools/appimage"
            directory.mkdir(parents=True)
            (directory / "appimagetool-1.9.1-x86_64.AppImage").write_bytes(b"not a trusted tool")
            with patch.object(packaging, "ROOT", root), patch.object(packaging.urllib.request, "urlopen") as download:
                with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                    packaging.fetch_tool()
                download.assert_not_called()

    def test_container_export_enforces_baseline_and_preserves_previous_artifact(self):
        for glibc, accepted in (("2.36", True), ("2.44", False)):
            with self.subTest(glibc=glibc), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary)
                previous = output / builder.FILES[0]
                previous.write_bytes(b"previous working image")
                payload = b"new image"
                files = {builder.FILES[0]: payload,
                         builder.FILES[1]: hashlib.sha256(payload).hexdigest().encode() + b"  image\n",
                         builder.FILES[2]: json.dumps({"architecture": "x86_64", "glibc_minimum": glibc}).encode()}

                def container(engine, image, mounts, command):
                    if image == builder.TOOLCHAIN:
                        destination = next(host for host, guest, mode in mounts if guest == "/output")
                        for name, contents in files.items():
                            (destination / name).write_bytes(contents)

                def build_toolchain(arguments, **kwargs):
                    context = Path(arguments[-1])
                    self.assertEqual({str(path.relative_to(context)) for path in context.rglob("*") if path.is_file()},
                                     {"Dockerfile", "tools/install_dotnet.py"})
                    return subprocess.CompletedProcess(arguments, 0)

                with patch.object(builder.subprocess, "run", side_effect=build_toolchain) as commands, \
                        patch.object(builder, "run_container", side_effect=container), \
                        patch("builtins.print"):
                    if accepted:
                        builder.build("docker", output)
                        self.assertEqual(previous.read_bytes(), payload)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "baseline"):
                            builder.build("docker", output)
                        self.assertEqual(previous.read_bytes(), b"previous working image")
                    self.assertEqual(commands.call_count, 1, "only the toolchain is built as an image")
                    self.assertIn(builder.TOOLCHAIN, commands.call_args.args[0])
                    self.assertEqual(sorted(path.name for path in output.iterdir()),
                                     sorted(builder.FILES) if accepted else [builder.FILES[0]])

    def test_disposable_container_cleanup_on_success_failure_and_interrupt(self):
        for failure in (None, subprocess.CalledProcessError(1, ["docker", "run"]), KeyboardInterrupt()):
            with self.subTest(failure=failure):
                def engine(arguments, **kwargs):
                    if arguments[1] == "run" and failure:
                        raise failure
                    return subprocess.CompletedProcess(arguments, 0)

                with patch.object(builder.subprocess, "run", side_effect=engine) as commands:
                    if failure:
                        with self.assertRaises(type(failure)):
                            builder.run_container("docker", "trusted-image", [], ["test"])
                    else:
                        builder.run_container("docker", "trusted-image", [], ["test"])
                launched = commands.call_args_list[0].args[0]
                name = launched[launched.index("--name") + 1]
                self.assertIn("--rm", launched)
                self.assertEqual(commands.call_args_list[-1].args[0], ["docker", "rm", "--force", name])

    def test_snapshot_excludes_game_tools_artifacts_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "source"
            destination = Path(temporary) / "snapshot"
            for name in ("Makefile", "src/main.cpp", ".tools/dotnet10/dotnet", "osu.AppImage",
                         "artifacts/private/client.realm", "tests/osu-indexer-tests/bin/test.dll"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test")
            (root / "src/private.cpp").symlink_to(root / "artifacts/private/client.realm")
            builder.source_snapshot(root, destination)
            self.assertEqual({str(path.relative_to(destination)) for path in destination.rglob("*") if path.is_file()},
                             {"Makefile", "src/main.cpp"})

    def test_toolchain_cleanup_never_prunes_global_docker_state(self):
        with patch.object(builder.subprocess, "run") as commands:
            builder.cleanup("docker")
        commands.assert_called_once_with(["docker", "image", "rm", builder.TOOLCHAIN], check=True)

    def test_failed_build_keeps_release_and_removes_host_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            for name in builder.FILES:
                (output / name).write_bytes(b"previous release")
            with patch.object(builder.subprocess, "run"), \
                    patch.object(builder, "run_container", side_effect=subprocess.CalledProcessError(1, ["test"])):
                with self.assertRaises(subprocess.CalledProcessError):
                    builder.build("docker", output)
            self.assertEqual(sorted(path.name for path in output.iterdir()), sorted(builder.FILES))
            self.assertTrue(all((output / name).read_bytes() == b"previous release" for name in builder.FILES))


if __name__ == "__main__":
    unittest.main()
