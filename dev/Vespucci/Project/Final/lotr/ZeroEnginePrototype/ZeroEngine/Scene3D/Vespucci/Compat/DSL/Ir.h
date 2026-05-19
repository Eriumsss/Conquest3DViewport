// Ir.h
// =============================================================================
// IR — LOWERED FORM OF DSL RULES, FED INTO THE BYTECODE GENERATOR
// =============================================================================
// Written by: Eriumsss
//
// We do NOT execute the AST directly. Two reasons:
//   1. The AST has the lazy text refs from the source buffer. The
//      source buffer MIGHT die before the runtime ranker fires (we
//      keep it alive for now, but I do not trust future-me to never
//      drop it).
//   2. AST walks have switch-on-enum overhead per node visited. The
//      ranker fires this per (sourceType, eventName) lookup, which
//      can be tens of thousands of calls per editor frame on a busy
//      level. Pre-lowered IR is just opcodes + immediate operands —
//      one indirect jump per opcode, no per-node dispatch.
//
// IR is a flat array of 32-bit opcodes plus a parallel const pool
// for strings and floats. Each rule lowers to one IRProgram. The
// CompatibilityMatrix holds a vector of IRPrograms and the runtime
// VM executes them in priority order against a (source, event,
// target, action) query.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_IR_H_
#define VESPUCCI_COMPAT_DSL_IR_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Compat {
namespace DSL {

enum IROp {
    OP_NOP        = 0,
    OP_PUSH_BOOL  = 1,    // imm: 0 or 1
    OP_PUSH_INT   = 2,    // imm: i32 (loose for our 32-bit IR; large constants go through const pool)
    OP_PUSH_FLOAT = 3,    // imm: const-pool index for f64
    OP_PUSH_STR   = 4,    // imm: const-pool index for string

    OP_ADD        = 10,
    OP_SUB        = 11,
    OP_MUL        = 12,
    OP_DIV        = 13,
    OP_MOD        = 14,
    OP_NEG        = 15,
    OP_NOT        = 16,
    OP_AND        = 17,
    OP_OR         = 18,
    OP_EQ         = 19,
    OP_NEQ        = 20,
    OP_LT         = 21,
    OP_LE         = 22,
    OP_GT         = 23,
    OP_GE         = 24,

    OP_CALL       = 30,   // imm: stdlib id; second imm: arg count
    OP_RETURN     = 31,
    OP_JUMP       = 32,   // imm: relative offset
    OP_JUMP_IF_F  = 33,   // pop, jump if popped is false; imm offset

    // Wire-spec match opcode. Pulled into IR so the matrix evaluator
    // does not have to interpret separate metadata. Imm: const-pool
    // indices for sourceType / eventName / targetType / actionName.
    // Wildcards encoded as a special pool index (0).
    OP_MATCH_WIRE = 40,

    // Result emitters. Final opcode of every rule.
    OP_RES_ALLOW  = 50,
    OP_RES_DENY   = 51,
    OP_RES_WEIGHT = 52,   // pops a float, attaches as weight bonus/penalty
    OP_RES_REASON = 53    // imm: const-pool index for reason string
};

// Stdlib function ids — bytecode-stable. New entries append at END.
enum StdlibId {
    SL_SAME_LAYER       = 1,
    SL_SAME_PARENT      = 2,
    SL_DIST_LESS_THAN   = 3,
    SL_SPATIAL          = 4,
    SL_DEPRECATED       = 5,
    SL_HAS_EVENT_COUNT  = 6,
    SL_HAS_TRAIT        = 7,
    SL_NAME_MATCHES     = 8,
    SL_MIN              = 9,
    SL_MAX              = 10,
    SL_ABS              = 11,
    SL_FLOOR            = 12,
    SL_CEIL             = 13
};

// Lowered program. One per rule.
struct IRProgram {
    std::vector<u32>          ops;         // packed opcodes — multi-word ops chain consecutively
    std::vector<f64>          floatPool;
    std::vector<std::string>  stringPool;  // owns the strings — IR outlives the AST/source

    // Header metadata for the matrix evaluator.
    bool      hasSpec;       // true if the program contains a MATCH_WIRE
    i32       reasonStringIdx; // -1 if no reason attached
    f32       weightBonus;     // statically known weight, 0.0 if dynamic
    Core::StringRef name;      // rule name for debug overlay (points into corpus arena)
};

// Encode helpers - written by the IR lowerer, read by the VM.
inline u32 EncodeImm32_0() { return 0u; }
inline u32 EncodeOpAndImm(IROp op, u32 imm) {
    // Low 8 bits = op, top 24 bits = imm. Imm overflow goes to const pool.
    return ((u32)op & 0xFFu) | (imm << 8);
}
inline IROp  DecodeOp(u32 instr)  { return (IROp)(instr & 0xFFu); }
inline u32   DecodeImm(u32 instr) { return instr >> 8; }

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_IR_H_
