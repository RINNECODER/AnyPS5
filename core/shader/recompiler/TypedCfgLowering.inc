private:
    std::unordered_map<std::uint32_t, std::uint32_t> ldsPendingDone;

    void AppendLdsPendingInstruction(spirv_cross::SPIRBlock& block, spv::Op opcode,
                                     const std::array<std::uint32_t, 4>& operands) {
        if (ir.spirv.size() > std::numeric_limits<std::uint32_t>::max() - 5u)
            Fail("LDS pending lowering exceeds native instruction offsets");
        spirv_cross::Instruction instruction{};
        instruction.op = static_cast<std::uint16_t>(opcode);
        instruction.count = 5;
        instruction.offset = static_cast<std::uint32_t>(ir.spirv.size()) + 1u;
        instruction.length = 4;
        ir.spirv.push_back((5u << 16u) | static_cast<std::uint32_t>(opcode));
        ir.spirv.insert(ir.spirv.end(), operands.begin(), operands.end());
        block.ops.push_back(instruction);
    }

public:
    std::optional<std::uint32_t> LdsPendingDone(std::uint32_t casResult) const {
        const auto found = ldsPendingDone.find(casResult);
        if (found == ldsPendingDone.end()) return std::nullopt;
        return found->second;
    }

    void PrepareLdsUniformPending() {
        const auto& protocols = LdsLockProtocols();
        if (protocols.empty()) return;
        if (!ldsPendingDone.empty()) Fail("LDS pending lowering was already applied");
        if (protocols.size() > (std::numeric_limits<std::uint32_t>::max() - ir.ids.size()) / 6u ||
            protocols.size() > (std::numeric_limits<std::uint32_t>::max() - ir.spirv.size()) / 10u)
            Fail("LDS pending lowering exceeds native compiler capacity");
        std::unordered_set<std::uint32_t> headers;
        std::unordered_set<std::uint32_t> compares;
        for (const auto& protocol : protocols) {
            if (!headers.insert(protocol.header).second || !compares.insert(protocol.casResult).second)
                Fail("LDS pending lowering has overlapping protocol metadata");
            const auto& header = get<spirv_cross::SPIRBlock>(protocol.header);
            const auto& body = get<spirv_cross::SPIRBlock>(protocol.body);
            const auto& critical = get<spirv_cross::SPIRBlock>(protocol.critical);
            const auto& after = get<spirv_cross::SPIRBlock>(protocol.after);
            const auto& continuation = get<spirv_cross::SPIRBlock>(protocol.continuation);
            const auto& result = get<spirv_cross::SPIRVariable>(protocol.oldResultPhi);
            const auto& resultType = get<spirv_cross::SPIRType>(protocol.oldResultType);
            const auto& successType = get<spirv_cross::SPIRType>(protocol.successType);
            if (header.merge != spirv_cross::SPIRBlock::MergeLoop ||
                header.terminator != spirv_cross::SPIRBlock::Direct || header.next_block != protocol.body ||
                header.merge_block != protocol.merge || header.continue_block != protocol.continuation ||
                !header.ops.empty() || !header.phi_variables.empty() ||
                body.merge != spirv_cross::SPIRBlock::MergeSelection ||
                body.terminator != spirv_cross::SPIRBlock::Select || body.condition != protocol.successBool ||
                body.true_block != protocol.critical || body.false_block != protocol.after ||
                body.next_block != protocol.after ||
                critical.terminator != spirv_cross::SPIRBlock::Direct || critical.next_block != protocol.after ||
                after.terminator != spirv_cross::SPIRBlock::Select || after.condition != protocol.successBool ||
                after.true_block != protocol.merge || after.false_block != protocol.continuation ||
                !after.ops.empty() || after.phi_variables.size() != 2u ||
                continuation.terminator != spirv_cross::SPIRBlock::Direct || continuation.next_block != protocol.header ||
                !continuation.ops.empty() || !continuation.phi_variables.empty() ||
                !result.phi_variable || result.storage != spv::StorageClassFunction || result.basetype != protocol.oldResultType ||
                resultType.basetype != spirv_cross::SPIRType::UInt || resultType.width != 32u || resultType.vecsize != 2u ||
                resultType.columns != 1u || !resultType.array.empty() || resultType.parent_type == 0u ||
                successType.basetype != spirv_cross::SPIRType::Boolean || successType.vecsize != 1u ||
                successType.columns != 1u || !successType.array.empty())
                Fail("LDS pending lowering no longer matches the recognized native protocol");
            const auto& scalarType = get<spirv_cross::SPIRType>(resultType.parent_type);
            if (scalarType.basetype != spirv_cross::SPIRType::UInt || scalarType.width != 32u || scalarType.vecsize != 1u ||
                scalarType.columns != 1u || !scalarType.array.empty())
                Fail("LDS pending lowering requires the original unsigned scalar scope type");
            bool successIncoming = false, failureIncoming = false;
            for (const auto& phi : after.phi_variables) {
                if (phi.function_variable != protocol.oldResultPhi)
                    Fail("LDS pending lowering encountered another result Phi");
                successIncoming |= phi.parent == protocol.critical && phi.local_variable == protocol.criticalOldResult;
                failureIncoming |= phi.parent == protocol.body && phi.local_variable == protocol.zeroResult;
            }
            if (!successIncoming || !failureIncoming)
                Fail("LDS pending lowering has different original result Phi edges");
        }
        for (const auto& protocol : protocols) {
            const auto ids = ir.increase_bound_by(6u);
            const auto falseId = ids;
            const auto scopeId = ids + 1u;
            const auto doneId = ids + 2u;
            const auto savedOldId = ids + 3u;
            const auto nextDoneId = ids + 4u;
            const auto allDoneId = ids + 5u;
            const auto scalarType = get<spirv_cross::SPIRType>(protocol.oldResultType).parent_type;
            set<spirv_cross::SPIRConstant>(falseId, protocol.successType, 0u, false);
            set<spirv_cross::SPIRConstant>(scopeId, scalarType, static_cast<std::uint32_t>(spv::ScopeSubgroup), false);
            set<spirv_cross::SPIRVariable>(doneId, protocol.successType, spv::StorageClassFunction).phi_variable = true;
            set<spirv_cross::SPIRVariable>(savedOldId, protocol.oldResultType, spv::StorageClassFunction).phi_variable = true;
            auto& function = get<spirv_cross::SPIRFunction>(protocol.function);
            function.add_local_variable(doneId);
            function.add_local_variable(savedOldId);
            auto& header = get<spirv_cross::SPIRBlock>(protocol.header);
            header.phi_variables.push_back({falseId, protocol.preheader, doneId});
            header.phi_variables.push_back({nextDoneId, protocol.continuation, doneId});
            header.phi_variables.push_back({protocol.zeroResult, protocol.preheader, savedOldId});
            header.phi_variables.push_back({protocol.oldResultPhi, protocol.continuation, savedOldId});
            auto& after = get<spirv_cross::SPIRBlock>(protocol.after);
            for (auto& phi : after.phi_variables)
                if (phi.parent == protocol.body && phi.function_variable == protocol.oldResultPhi)
                    phi.local_variable = savedOldId;
            AppendLdsPendingInstruction(after, spv::OpLogicalOr,
                {protocol.successType, nextDoneId, doneId, protocol.successBool});
            AppendLdsPendingInstruction(after, spv::OpGroupNonUniformAll,
                {protocol.successType, allDoneId, scopeId, nextDoneId});
            forced_temporaries.insert(nextDoneId);
            forced_temporaries.insert(allDoneId);
            after.condition = allDoneId;
            ldsPendingDone.emplace(protocol.casResult, doneId);
        }
    }
