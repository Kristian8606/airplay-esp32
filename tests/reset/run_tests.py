#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parent;root=p.parents[1]
with tempfile.TemporaryDirectory(prefix="web-reset-") as tmp:
 out=Path(tmp)/"reset"
 subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-fsanitize=undefined","-fno-sanitize-recover=all","-I"+str(p/"stubs"),"-I"+str(root/"tests/homekit/stubs"),"-I"+str(root/"components/web_server/include"),"-I"+str(root/"managed_components/espressif__cjson/cJSON"),"-I"+str(root/"main"),str(p/"reset_test.c"),str(root/"main/device_reset.c"),str(root/"managed_components/espressif__cjson/cJSON/cJSON.c"),"-o",str(out)],check=True)
 for case in ["boot","auth","bad","stop","homekit","missing","open-fail","key-fail","commit-fail","factory","storage-fail","nvs-fail","no-storage"]:
  subprocess.run([str(out),case],check=True,timeout=5)
