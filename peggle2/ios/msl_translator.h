// Translates Xenos shader microcode into Metal Shading Language via rex::graphics::ShaderTranslator.
#pragma once

#include <cstdint>
#include <string>
#include <rex/graphics/pipeline/shader/translator.h>

namespace rex_ios {

struct MslShaderInfo {
  bool isPixel = false;
  uint32_t textureSlots = 0;
  uint32_t textureDimensions[32]{};
  uint32_t colorTargets = 0;
  bool writesDepth = false;
  uint32_t interpolators = 0;
  uint32_t inputsRead = 0;
  uint32_t positionSlot = 0;
  uint32_t positionOffset = 0;
  uint32_t positionStride = 0;
  std::string errors;
};

std::string FragmentEntry(const MslShaderInfo& info);

class MslTranslator : public rex::graphics::ShaderTranslator {
 public:
  std::string Translate(rex::graphics::Shader& shader, MslShaderInfo& info);

 protected:
  void EmitTranslationError(const char* message, bool is_fatal) override;
  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;
  void ProcessControlFlowInstructionBegin(uint32_t cf_index) override;
  void ProcessExecInstructionBegin(const rex::graphics::ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const rex::graphics::ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(
      const rex::graphics::ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const rex::graphics::ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const rex::graphics::ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const rex::graphics::ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const rex::graphics::ParsedJumpInstruction& instr) override;
  void ProcessVertexFetchInstruction(
      const rex::graphics::ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const rex::graphics::ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(const rex::graphics::ParsedAluInstruction& instr,
                             uint8_t memexport_eM_potentially_written_before) override;

 private:
  std::string Operand(const rex::graphics::InstructionOperand& op);
  std::string ScalarComponent(const rex::graphics::InstructionOperand& op, uint32_t component);
  void StoreResult(const rex::graphics::InstructionResult& result, const std::string& value);
  struct Fetched {
    uint32_t slot = 0;
    uint32_t offset = 0;
    uint32_t stride = 0;
  };
  Fetched PositionFetch(const rex::graphics::ParsedVertexFetchInstruction& instr) const;
  Fetched PassThroughPosition(const rex::graphics::ParsedAluInstruction& instr) const;
  void Line(const std::string& text);
  std::string Condition(uint32_t bool_index, bool condition, bool predicated);

  Fetched fetched_[64]{};
  Fetched position_{};
  uint64_t writtenRegisters_ = 0;
  uint32_t positionWrites_ = 0;
  bool branches_ = false;
  std::string body_;
  int indent_ = 0;
  bool inExecCondition_ = false;
  MslShaderInfo* info_ = nullptr;
};

}  // namespace rex_ios
