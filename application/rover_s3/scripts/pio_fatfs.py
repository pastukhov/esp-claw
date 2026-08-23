from pathlib import Path
import csv
import subprocess
import sys

Import("env")


def parse_size(value):
    text = value.strip()
    return int(text, 16) if text.lower().startswith("0x") else int(text)


def partition_info(project_dir, name):
    """Return (offset, size) for a named row in partitions.csv."""
    partitions = project_dir / "partitions.csv"
    with partitions.open(newline="") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.lstrip().startswith("#")):
            if len(row) >= 5 and row[0].strip() == name:
                return parse_size(row[3]), parse_size(row[4])
    raise RuntimeError("partition '%s' not found in partitions.csv" % name)


def generate_storage_image(*args, **kwargs):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    build_dir = Path(env.subst("$BUILD_DIR"))
    image_dir = project_dir / "fatfs_image"
    out_file = build_dir / "storage.bin"
    build_dir.mkdir(parents=True, exist_ok=True)

    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    if not framework_dir:
        raise RuntimeError("framework-espidf package is not installed")
    generator = Path(framework_dir) / "components" / "fatfs" / "wl_fatfsgen.py"

    try:
        import construct  # noqa: F401
    except ImportError as exc:
        raise RuntimeError("Install virtualenv dependencies first: .venv/bin/python -m pip install -r requirements.txt") from exc

    _, storage_size = partition_info(project_dir, "storage")
    cmd = [
        sys.executable,
        str(generator),
        str(image_dir),
        "--long_name_support",
        "--use_default_datetime",
        "--partition_size",
        str(storage_size),
        "--output_file",
        str(out_file),
        "--sector_size",
        "4096",
    ]
    subprocess.check_call(cmd)


def generate_srmodels_image(*args, **kwargs):
    """Build esp-sr's `srmodels_bin` ninja target (ALL-tagged in its
    CMakeLists.txt, but PlatformIO's own build step never asks ninja for
    the blanket "ALL" target, so it's otherwise silently skipped). Must
    run after the main build so build.ninja + IDF_PATH are already set up
    by PlatformIO's own environment."""
    build_dir = Path(env.subst("$BUILD_DIR"))
    out_file = build_dir / "srmodels" / "srmodels.bin"

    ninja_dir = env.PioPlatform().get_package_dir("tool-ninja")
    if not ninja_dir:
        raise RuntimeError("tool-ninja package is not installed")
    ninja = Path(ninja_dir) / "ninja"

    subprocess.check_call(
        [str(ninja), "-C", str(build_dir), "srmodels_bin"],
        env=env["ENV"],
    )
    if not out_file.is_file():
        raise RuntimeError(
            "srmodels_bin target ran but %s wasn't produced "
            "(check CONFIG_SR_WN_*/CONFIG_SR_MN_* selections in sdkconfig)" % out_file
        )


project_dir = Path(env.subst("$PROJECT_DIR"))
storage_offset, _ = partition_info(project_dir, "storage")
model_offset, _ = partition_info(project_dir, "model")

generate_storage_image()
env.AddPreAction("upload", generate_storage_image)
env.AddPreAction("upload", generate_srmodels_image)
env.AddPreAction("buildprog", generate_srmodels_image)

env.Append(
    FLASH_EXTRA_IMAGES=[
        (hex(storage_offset), "$BUILD_DIR/storage.bin"),
        (hex(model_offset), "$BUILD_DIR/srmodels/srmodels.bin"),
    ]
)
