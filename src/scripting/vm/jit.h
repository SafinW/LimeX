
#pragma once

#include "vmintern.h"

JitFuncPtr JitCompile(VMScriptFunction *func);
void JitCleanUp(VMScriptFunction *func);
