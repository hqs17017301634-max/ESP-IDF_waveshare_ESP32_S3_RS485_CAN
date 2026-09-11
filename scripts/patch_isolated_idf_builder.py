"""Apply two Windows path fixes only to the project-local PlatformIO 7 builder.

No ESP-IDF sources or global PlatformIO files are modified. Record before/after
hashes so the build workaround is reviewable and reproducible.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
path = ROOT/'_tools/pio-core/platforms/espressif32/builder/frameworks/espidf.py'
original = path.read_bytes()
text = original.decode('utf-8').replace('\r\n', '\n')
marker = '# S3_RX_LOCAL_WINDOWS_BUILDER_FIX'
if marker not in text:
    old = 'obj_path, os.path.relpath(src_path, components_dir)'
    assert text.count(old) == 1, 'PlatformIO component path anchor changed'
    text = text.replace(old, 'obj_path, os.path.relpath(str(Path(src_path).resolve()), components_dir)')
    old = '        env.VerboseAction(cmd, "Generating project linker script $TARGET"),'
    assert text.count(old) == 1, 'PlatformIO ldgen action anchor changed'
    text = text.replace(old, '        env.VerboseAction(run_s3_ldgen, "Generating project linker script $TARGET"),')
    anchor = '    return env.Command(\n        os.path.join("$BUILD_DIR", "sections.ld"),'
    assert text.count(anchor) == 1
    helper = '''    # S3_RX_LOCAL_WINDOWS_BUILDER_FIX
    def run_s3_ldgen(target, source, env):
        arguments = [
            "--input", source[0].get_abspath(),
            "--config", args["config"],
            "--fragments", *linker_script_fragments,
            "--output", target[0].get_abspath(),
            "--kconfig", args["kconfig"],
            "--env-file", env.subst(args["env_file"]),
            "--libraries-file", args["libraries_list"],
            "--objdump", args["objdump"],
        ]
        response = env.subst("$BUILD_DIR/ldgen-arguments.json")
        with open(response, "w", encoding="utf-8") as fp:
            json.dump({"script": args["script"], "arguments": arguments}, fp)
        launcher = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "run_ldgen_args.py")
        return subprocess.call(
            [env.subst("$ESPIDF_PYTHONEXE"), launcher, response],
            env={str(k): str(v) for k, v in env["ENV"].items()},
        )

'''
    text = text.replace(anchor, helper+anchor)
    path.with_suffix('.py.before-s3-rx').write_bytes(original)
    path.write_text(text, encoding='utf-8')
    report = {'builder': str(path), 'before_sha256': hashlib.sha256(original).hexdigest(),
              'after_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'changes': ['normalize both sides before component relpath', 'ldgen JSON argument launcher']}
    (ROOT/'_tools/idf-builder-patch.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('Isolated Windows builder fixes ready')
