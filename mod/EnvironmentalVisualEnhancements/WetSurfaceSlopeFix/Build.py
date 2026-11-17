# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3]/'core/ShaderPatch'))
from BuildRecipe import build

if __name__ == '__main__':
    build(Path(__file__).resolve().parent)
