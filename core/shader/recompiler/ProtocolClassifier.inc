public:
    struct LdsLockProtocol {
        std::uint32_t casResult = 0, lockPointer = 0, header = 0, body = 0, critical = 0;
        std::uint32_t after = 0, continuation = 0, merge = 0, preheader = 0;
        std::uint32_t oldResultPhi = 0, successBool = 0, oldResultType = 0;
        std::uint32_t criticalOldResult = 0, zeroResult = 0, successType = 0;
        std::uint32_t function = 0, base = 0, lockSlot = 0;
        std::uint32_t initializerPointer = 0, initializerBlock = 0;
        std::size_t initializerStoreWordOffset = 0, initializerBarrierWordOffset = 0;
        std::size_t unlockWordOffset = 0, casWordOffset = 0;
    };

    const std::vector<LdsLockProtocol>& LdsLockProtocols() const { return ldsLockProtocols; }

private:
    std::unordered_set<std::uint32_t> electedLockCasResults;
    std::vector<LdsLockProtocol> ldsLockProtocols;

public:
    void PrepareLdsLockElection(const RecompileResult& guest) {
        electedLockCasResults.clear();
        ldsLockProtocols.clear();
        struct Instruction {
            spv::Op op;
            std::vector<std::uint32_t> operands;
            std::uint32_t function = 0, block = 0, result = 0, type = 0;
            std::size_t prefix = 0;
            std::size_t wordOffset = 0;
        };
        struct Block {
            std::uint32_t function = 0;
            std::vector<std::size_t> instructions;
            std::vector<std::uint32_t> successors, predecessors;
        };
        struct Protocol {
            std::uint32_t result, pointer, base;
            std::size_t compare, release;
            LdsLockProtocol metadata;
        };
        using Use = std::pair<std::size_t, std::size_t>;
        std::vector<Instruction> instructions;
        std::unordered_map<std::uint32_t, std::size_t> definitions;
        std::unordered_map<std::uint32_t, Block> blocks;
        std::unordered_map<std::uint32_t, std::uint32_t> entries;
        std::unordered_set<std::uint32_t> entryFunctions;
        const auto words = guest.spirv.Words();
        if (words.size() < 5 || words[0] != 0x07230203u || words[3] == 0) return;
        std::uint32_t function = 0, block = 0;
        for (std::size_t offset = 5; offset < words.size();) {
            const auto count = words[offset] >> 16u;
            if (!count || count > words.size() - offset) return;
            Instruction instruction{};
            instruction.wordOffset = offset;
            instruction.op = static_cast<spv::Op>(words[offset] & 0xffffu);
            instruction.operands.assign(words.begin() + offset + 1, words.begin() + offset + count);
            bool result = false, type = false;
            spv::HasResultAndType(instruction.op, &result, &type);
            instruction.prefix = std::size_t(result) + std::size_t(type);
            if (instruction.operands.size() < instruction.prefix) return;
            if (type) instruction.type = instruction.operands[0];
            if (result) {
                instruction.result = instruction.operands[type ? 1 : 0];
                if (!instruction.result || instruction.result >= words[3] ||
                    !definitions.emplace(instruction.result, instructions.size()).second) return;
            }
            if (instruction.op == spv::OpEntryPoint) {
                if (instruction.operands.size() < 3) return;
                entryFunctions.insert(instruction.operands[1]);
            }
            if (instruction.op == spv::OpFunction) {
                if (function || instruction.operands.size() != 4) return;
                function = instruction.result;
            }
            if (instruction.op == spv::OpLabel) {
                if (!function || instruction.operands.size() != 1) return;
                block = instruction.result;
                if (!blocks.emplace(block, Block{function, {}, {}, {}}).second) return;
                entries.try_emplace(function, block);
            }
            instruction.function = function;
            instruction.block = block;
            if (block && instruction.op != spv::OpFunctionEnd)
                blocks.at(block).instructions.push_back(instructions.size());
            instructions.push_back(std::move(instruction));
            if (instructions.back().op == spv::OpFunctionEnd) {
                if (!function || !instructions.back().operands.empty()) return;
                function = block = 0;
            }
            offset += count;
        }
        if (function) return;
        const auto definition = [&](std::uint32_t id) -> const Instruction* {
            const auto found = definitions.find(id);
            return found == definitions.end() ? nullptr : &instructions[found->second];
        };
        const auto unsigned32Type = [&](std::uint32_t id) {
            const auto* d = definition(id);
            return d && d->op == spv::OpTypeInt && d->operands.size() == 3 &&
                d->operands[1] == 32 && d->operands[2] == 0;
        };
        const auto constant = [&](std::uint32_t id) -> std::optional<std::uint32_t> {
            const auto* d = definition(id);
            if (!d || d->op != spv::OpConstant || d->operands.size() != 3 || !unsigned32Type(d->type))
                return std::nullopt;
            return d->operands[2];
        };
        const auto constantIs = [&](std::uint32_t id, std::uint32_t value) {
            const auto found = constant(id);
            return found && *found == value;
        };
        const auto uses = [&](std::uint32_t id) {
            std::vector<Use> found;
            for (std::size_t i = 0; i < instructions.size(); ++i) {
                const auto& instruction = instructions[i];
                if (!instruction.function) continue;
                for (std::size_t operand = instruction.prefix; operand < instruction.operands.size(); ++operand)
                    if (instruction.operands[operand] == id) found.emplace_back(i, operand);
            }
            return found;
        };
        const auto exactUses = [&](std::uint32_t id, std::vector<Use> expected) {
            auto found = uses(id);
            std::sort(found.begin(), found.end());
            std::sort(expected.begin(), expected.end());
            return found == expected;
        };
        for (auto& [id, current] : blocks) {
            static_cast<void>(id);
            for (std::size_t position = 0; position + 1 < current.instructions.size(); ++position) {
                switch (instructions[current.instructions[position]].op) {
                case spv::OpBranch:
                case spv::OpBranchConditional:
                case spv::OpSwitch:
                case spv::OpReturn:
                case spv::OpReturnValue:
                case spv::OpUnreachable:
                case spv::OpKill:
                case spv::OpTerminateInvocation:
                    return;
                default:
                    break;
                }
            }
            const auto& terminal = instructions[current.instructions.back()];
            const auto& a = terminal.operands;
            if (terminal.op == spv::OpBranch && a.size() == 1) current.successors = {a[0]};
            else if (terminal.op == spv::OpBranchConditional && a.size() == 3)
                current.successors = {a[1], a[2]};
            else if (terminal.op == spv::OpReturn && a.empty()) {}
            else if (terminal.op == spv::OpReturnValue && a.size() == 1) {}
            else if (terminal.op == spv::OpUnreachable && a.empty()) {}
            else return;
            for (const auto successor : current.successors) {
                const auto target = blocks.find(successor);
                if (target == blocks.end() || target->second.function != current.function) return;
                target->second.predecessors.push_back(id);
            }
        }
        const auto reachable = [&](std::uint32_t target, std::uint32_t excluded,
                                   std::uint32_t excludedFrom = 0, std::uint32_t excludedTo = 0) {
            const auto targetBlock = blocks.find(target);
            if (targetBlock == blocks.end()) return false;
            const auto entry = entries.find(targetBlock->second.function);
            if (entry == entries.end()) return false;
            std::unordered_set<std::uint32_t> seen{excluded};
            std::vector<std::uint32_t> pending{entry->second};
            while (!pending.empty()) {
                const auto next = pending.back();
                pending.pop_back();
                if (!seen.insert(next).second) continue;
                if (next == target) return true;
                for (const auto successor : blocks.at(next).successors)
                    if (next != excludedFrom || successor != excludedTo) pending.push_back(successor);
            }
            return false;
        };
        const auto dominates = [&](std::uint32_t dominator, std::uint32_t target) {
            return reachable(target, 0) && !reachable(target, dominator);
        };
        const auto predecessorSetIs = [&](std::uint32_t id, std::vector<std::uint32_t> expected) {
            const auto found = blocks.find(id);
            if (found == blocks.end()) return false;
            auto actual = found->second.predecessors;
            std::sort(actual.begin(), actual.end());
            std::sort(expected.begin(), expected.end());
            return actual == expected;
        };
        const auto boundedLow = [&](std::uint32_t id) {
            if (const auto value = constant(id)) return *value < std::numeric_limits<std::uint32_t>::max();
            const auto* d = definition(id);
            if (!d || d->op != spv::OpShiftRightLogical || d->operands.size() != 4 || !unsigned32Type(d->type))
                return false;
            const auto* source = definition(d->operands[2]);
            const auto shift = constant(d->operands[3]);
            return source && unsigned32Type(source->type) && shift && *shift > 0 && *shift < 32;
        };
        const auto plusOne = [&](std::uint32_t high, std::uint32_t low) {
            const auto* d = definition(high);
            return d && d->op == spv::OpIAdd && d->operands.size() == 4 && unsigned32Type(d->type) &&
                d->operands[2] == low && constantIs(d->operands[3], 1) && boundedLow(low);
        };
        const auto boundedAccess = [&](std::uint32_t index, std::uint32_t accessBlock, std::uint32_t slot) {
            if (const auto value = constant(index)) return *value < slot;
            const auto* indexDefinition = definition(index);
            if (!indexDefinition || !unsigned32Type(indexDefinition->type)) return false;
            for (const auto& [id, guardBlock] : blocks) {
                const auto& branch = instructions[guardBlock.instructions.back()];
                if (branch.op != spv::OpBranchConditional) continue;
                const auto* condition = definition(branch.operands[0]);
                if (!condition || condition->op != spv::OpULessThan || condition->operands.size() != 4 ||
                    !constantIs(condition->operands[3], slot)) continue;
                const auto* conditionType = definition(condition->type);
                if (!conditionType || conditionType->op != spv::OpTypeBool || conditionType->operands.size() != 1) continue;
                const auto guarded = condition->operands[2];
                if (guarded != index && !plusOne(guarded, index)) continue;
                if (branch.operands[1] != branch.operands[2] && dominates(id, accessBlock) &&
                    dominates(branch.operands[1], accessBlock) && !dominates(branch.operands[2], accessBlock) &&
                    reachable(accessBlock, 0) && !reachable(accessBlock, 0, id, branch.operands[1]))
                    return true;
            }
            return false;
        };
        const auto numericType = [&](auto&& self, std::uint32_t id, unsigned depth) -> bool {
            if (depth > 2) return false;
            const auto* d = definition(id);
            if (!d) return false;
            if (d->op == spv::OpTypeBool) return d->operands.size() == 1;
            if (d->op == spv::OpTypeInt)
                return d->operands.size() == 3 && (d->operands[1] == 32 || d->operands[1] == 64) && d->operands[2] <= 1;
            return d->op == spv::OpTypeVector && d->operands.size() == 3 &&
                d->operands[2] == 2 && self(self, d->operands[1], depth + 1);
        };
        const auto pure = [&](const Instruction& instruction) {
            if (!numericType(numericType, instruction.type, 0)) return false;
            const auto& a = instruction.operands;
            std::size_t valuesEnd = a.size();
            switch (instruction.op) {
            case spv::OpBitcast:
            case spv::OpNot:
            case spv::OpLogicalNot:
                if (a.size() != 3) return false;
                break;
            case spv::OpIAdd:
            case spv::OpISub:
            case spv::OpBitwiseAnd:
            case spv::OpBitwiseOr:
            case spv::OpBitwiseXor:
            case spv::OpLogicalAnd:
            case spv::OpLogicalOr:
            case spv::OpIEqual:
            case spv::OpINotEqual:
            case spv::OpUGreaterThan:
            case spv::OpUGreaterThanEqual:
            case spv::OpULessThan:
            case spv::OpULessThanEqual:
            case spv::OpSGreaterThan:
            case spv::OpSGreaterThanEqual:
            case spv::OpSLessThan:
            case spv::OpSLessThanEqual:
                if (a.size() != 4) return false;
                break;
            case spv::OpSelect:
                if (a.size() != 5) return false;
                break;
            case spv::OpCompositeExtract:
                if (a.size() != 4 || a[3] > 1) return false;
                valuesEnd = 3;
                break;
            default:
                return false;
            }
            for (std::size_t operand = 2; operand < valuesEnd; ++operand) {
                const auto* value = definition(a[operand]);
                if (!value || !numericType(numericType, value->type, 0)) return false;
            }
            return true;
        };
        const auto pointerBase = [&](std::uint32_t pointer, std::uint32_t& base, std::uint32_t& slot) {
            const auto* chain = definition(pointer);
            if (!chain || chain->op != spv::OpAccessChain || chain->operands.size() != 4) return false;
            const auto* pointerType = definition(chain->type);
            if (!pointerType || pointerType->op != spv::OpTypePointer || pointerType->operands.size() != 3 ||
                pointerType->operands[1] != spv::StorageClassWorkgroup || !unsigned32Type(pointerType->operands[2]))
                return false;
            base = chain->operands[2];
            const auto* variable = definition(base);
            if (!variable || variable->op != spv::OpVariable || variable->operands.size() != 3 ||
                variable->block || variable->operands[2] != spv::StorageClassWorkgroup) return false;
            const auto* arrayPointer = definition(variable->type);
            if (!arrayPointer || arrayPointer->op != spv::OpTypePointer || arrayPointer->operands.size() != 3 ||
                arrayPointer->operands[1] != spv::StorageClassWorkgroup) return false;
            const auto* array = definition(arrayPointer->operands[2]);
            if (!array || array->op != spv::OpTypeArray || array->operands.size() != 3 ||
                array->operands[1] != pointerType->operands[2]) return false;
            const auto extent = constant(array->operands[2]);
            if (!extent || *extent < 2) return false;
            slot = *extent - 1;
            return constantIs(chain->operands[3], slot);
        };
        std::unordered_map<std::uint32_t, Protocol> protocols;
        std::unordered_map<std::uint32_t, std::uint32_t> slots;
        for (std::size_t compare = 0; compare < instructions.size(); ++compare) {
            const auto& cas = instructions[compare];
            const auto& a = cas.operands;
            if (cas.op != spv::OpAtomicCompareExchange || a.size() != 8 || !unsigned32Type(cas.type) ||
                !cas.block || !entryFunctions.contains(cas.function) || !constantIs(a[3], spv::ScopeWorkgroup) ||
                !constantIs(a[4], spv::MemorySemanticsAcquireMask | spv::MemorySemanticsWorkgroupMemoryMask) ||
                !constantIs(a[5], spv::MemorySemanticsMaskNone) || !constantIs(a[6], 1) || !constantIs(a[7], 0))
                continue;
            std::uint32_t base = 0, slot = 0;
            if (!pointerBase(a[2], base, slot)) continue;
            const auto* lockPointer = definition(a[2]);
            if (!lockPointer || lockPointer->function != cas.function ||
                !dominates(lockPointer->block, cas.block)) continue;
            const auto& body = blocks.at(cas.block).instructions;
            if (body.size() != 5 || body[1] != compare) continue;
            const auto equal = body[2], selection = body[3], bodyBranch = body[4];
            const auto& eq = instructions[equal];
            const auto& sel = instructions[selection];
            const auto& branch = instructions[bodyBranch];
            const auto* boolean = definition(eq.type);
            if (eq.op != spv::OpIEqual || eq.operands.size() != 4 || eq.operands[2] != cas.result ||
                !boolean || boolean->op != spv::OpTypeBool || boolean->operands.size() != 1 ||
                !constantIs(eq.operands[3], 0) || sel.op != spv::OpSelectionMerge || sel.operands.size() != 2 ||
                sel.operands[1] != spv::SelectionControlMaskNone || branch.op != spv::OpBranchConditional ||
                branch.operands.size() != 3 || branch.operands[0] != eq.result || branch.operands[2] != sel.operands[0])
                continue;
            const auto critical = branch.operands[1], after = branch.operands[2];
            if (critical == after || !blocks.contains(critical) || !blocks.contains(after)) continue;
            const auto& post = blocks.at(after).instructions;
            if (post.size() != 3) continue;
            const auto& phi = instructions[post[1]];
            const auto& exit = instructions[post[2]];
            if (phi.op != spv::OpPhi || phi.operands.size() != 6 || phi.operands[3] != critical ||
                phi.operands[5] != cas.block || exit.op != spv::OpBranchConditional || exit.operands.size() != 3 ||
                exit.operands[0] != eq.result) continue;
            const auto merge = exit.operands[1], continuation = exit.operands[2];
            if (!blocks.contains(merge) || !blocks.contains(continuation) ||
                blocks.at(cas.block).predecessors.size() != 1) continue;
            const auto header = blocks.at(cas.block).predecessors[0];
            const auto& head = blocks.at(header).instructions;
            const auto& cont = blocks.at(continuation).instructions;
            if (head.size() != 3 || cont.size() != 2 || instructions[head[1]].op != spv::OpLoopMerge ||
                instructions[head[1]].operands != std::vector<std::uint32_t>{merge, continuation, spv::LoopControlMaskNone} ||
                instructions[head[2]].op != spv::OpBranch || instructions[head[2]].operands != std::vector<std::uint32_t>{cas.block} ||
                instructions[cont[1]].op != spv::OpBranch || instructions[cont[1]].operands != std::vector<std::uint32_t>{header} ||
                !predecessorSetIs(critical, {cas.block}) || !predecessorSetIs(after, {cas.block, critical}) ||
                !predecessorSetIs(continuation, {after}) || !predecessorSetIs(merge, {after})) continue;
            const auto& headPredecessors = blocks.at(header).predecessors;
            if (headPredecessors.size() != 2 || std::count(headPredecessors.begin(), headPredecessors.end(), continuation) != 1 ||
                !dominates(header, cas.block)) continue;
            const auto preheader = headPredecessors[0] == continuation ? headPredecessors[1] : headPredecessors[0];
            const auto preheaderBranch = blocks.at(preheader).instructions.back();
            if (instructions[preheaderBranch].op != spv::OpBranch ||
                instructions[preheaderBranch].operands != std::vector<std::uint32_t>{header} ||
                std::unordered_set<std::uint32_t>{preheader, header, cas.block, critical, after, continuation, merge}.size() != 7)
                continue;
            if (!exactUses(cas.result, {{equal, 2}}) || !exactUses(eq.result, {{bodyBranch, 0}, {post[2], 0}})) continue;
            const auto& section = blocks.at(critical).instructions;
            if (section.size() < 12) continue;
            const auto& lowPointer = instructions[section[1]];
            const auto& highPointer = instructions[section[2]];
            const auto& lowLoad = instructions[section[3]];
            const auto& highLoad = instructions[section[4]];
            const auto& old = instructions[section[5]];
            const auto& lowStore = instructions[section[section.size() - 4]];
            const auto& highStore = instructions[section[section.size() - 3]];
            const auto release = section[section.size() - 2];
            const auto& unlock = instructions[release];
            const auto& end = instructions[section.back()];
            if (lowPointer.op != spv::OpAccessChain || lowPointer.operands.size() != 4 ||
                highPointer.op != spv::OpAccessChain || highPointer.operands.size() != 4 ||
                lowPointer.operands[2] != base || highPointer.operands[2] != base || lowPointer.type != highPointer.type ||
                !plusOne(highPointer.operands[3], lowPointer.operands[3]) ||
                !boundedAccess(highPointer.operands[3], cas.block, slot) ||
                lowLoad.op != spv::OpLoad || lowLoad.operands != std::vector<std::uint32_t>{cas.type, lowLoad.result, lowPointer.result} ||
                highLoad.op != spv::OpLoad || highLoad.operands != std::vector<std::uint32_t>{cas.type, highLoad.result, highPointer.result} ||
                old.op != spv::OpCompositeConstruct || old.operands.size() != 4 ||
                old.operands[2] != lowLoad.result || old.operands[3] != highLoad.result || phi.operands[2] != old.result ||
                old.type != phi.type || !numericType(numericType, old.type, 0) ||
                lowStore.op != spv::OpStore || lowStore.operands.size() != 2 || lowStore.operands[0] != lowPointer.result ||
                highStore.op != spv::OpStore || highStore.operands.size() != 2 || highStore.operands[0] != highPointer.result ||
                unlock.op != spv::OpAtomicStore || unlock.operands.size() != 4 || unlock.operands[0] != a[2] ||
                !constantIs(unlock.operands[1], spv::ScopeWorkgroup) ||
                !constantIs(unlock.operands[2], spv::MemorySemanticsReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask) ||
                !constantIs(unlock.operands[3], 0) || end.op != spv::OpBranch ||
                end.operands != std::vector<std::uint32_t>{after}) continue;
            const auto* oldType = definition(old.type);
            const auto* zero = definition(phi.operands[4]);
            if (!oldType || oldType->op != spv::OpTypeVector || oldType->operands.size() != 3 ||
                oldType->operands[1] != cas.type || oldType->operands[2] != 2 || !zero ||
                zero->op != spv::OpConstantComposite || zero->type != old.type || zero->operands.size() != 4 ||
                !constantIs(zero->operands[2], 0) || !constantIs(zero->operands[3], 0)) continue;
            bool safe = true;
            for (std::size_t position = 6; position + 4 < section.size(); ++position)
                if (!pure(instructions[section[position]])) safe = false;
            const auto* nextLow = definition(lowStore.operands[1]);
            const auto* nextHigh = definition(highStore.operands[1]);
            if (!nextLow || !nextHigh || nextLow->op != spv::OpCompositeExtract || nextHigh->op != spv::OpCompositeExtract ||
                nextLow->operands.size() != 4 || nextHigh->operands.size() != 4 || nextLow->type != cas.type || nextHigh->type != cas.type ||
                nextLow->operands[2] != nextHigh->operands[2] || nextLow->operands[3] != 0 || nextHigh->operands[3] != 1 ||
                nextLow->block != critical || nextHigh->block != critical ||
                !exactUses(lowPointer.result, {{section[3], 2}, {section[section.size() - 4], 0}}) ||
                !exactUses(highPointer.result, {{section[4], 2}, {section[section.size() - 3], 0}}) ||
                !exactUses(a[2], {{compare, 2}, {release, 0}})) safe = false;
            if (!exactUses(header, {{preheaderBranch, 0}, {cont[1], 0}}) ||
                !exactUses(cas.block, {{head[2], 0}, {post[1], 5}}) ||
                !exactUses(critical, {{bodyBranch, 1}, {post[1], 3}}) ||
                !exactUses(after, {{selection, 0}, {bodyBranch, 2}, {section.back(), 0}}) ||
                !exactUses(continuation, {{head[1], 1}, {post[2], 2}}) ||
                !exactUses(merge, {{head[1], 0}, {post[2], 1}})) safe = false;
            const std::unordered_set<std::uint32_t> loopBlocks{header, cas.block, critical, after, continuation};
            for (const auto& [useIndex, operand] : uses(phi.result)) {
                const auto& use = instructions[useIndex];
                if (use.function != cas.function || loopBlocks.contains(use.block)) { safe = false; break; }
                if (use.op == spv::OpPhi) {
                    if (operand < 2 || (operand & 1u) || operand + 1 >= use.operands.size()) { safe = false; break; }
                    const auto incoming = use.operands[operand + 1];
                    if (!blocks.contains(incoming) || !dominates(merge, incoming) ||
                        std::find(blocks.at(incoming).successors.begin(), blocks.at(incoming).successors.end(), use.block) ==
                            blocks.at(incoming).successors.end()) { safe = false; break; }
                } else if (!dominates(merge, use.block)) { safe = false; break; }
            }
            for (std::size_t position = 1; position + 1 < section.size(); ++position) {
                const auto& value = instructions[section[position]];
                if (!value.result) continue;
                for (const auto& [useIndex, operand] : uses(value.result))
                    if (instructions[useIndex].block != critical &&
                        !(value.result == old.result && useIndex == post[1] && operand == 2)) safe = false;
            }
            if (!safe) continue;
            LdsLockProtocol metadata{};
            metadata.casResult = cas.result;
            metadata.lockPointer = a[2];
            metadata.header = header;
            metadata.body = cas.block;
            metadata.critical = critical;
            metadata.after = after;
            metadata.continuation = continuation;
            metadata.merge = merge;
            metadata.preheader = preheader;
            metadata.oldResultPhi = phi.result;
            metadata.successBool = eq.result;
            metadata.oldResultType = phi.type;
            metadata.criticalOldResult = old.result;
            metadata.zeroResult = phi.operands[4];
            metadata.successType = eq.type;
            metadata.function = cas.function;
            metadata.base = base;
            metadata.lockSlot = slot;
            metadata.unlockWordOffset = unlock.wordOffset;
            metadata.casWordOffset = cas.wordOffset;
            protocols.emplace(cas.result, Protocol{cas.result, a[2], base, compare, release, metadata});
            slots.emplace(base, slot);
        }
        for (const auto& [base, slot] : slots) {
            bool safe = true;
            std::optional<std::uint32_t> initializedFunction;
            std::uint32_t initializerPointer = 0, initializerBlock = 0;
            std::size_t initializerStoreWordOffset = 0, initializerBarrierWordOffset = 0;
            std::vector<std::uint32_t> selected;
            for (const auto& [id, protocol] : protocols)
                if (protocol.base == base) selected.push_back(id);
            for (const auto& [useIndex, operand] : uses(base)) {
                const auto& chain = instructions[useIndex];
                if (chain.op != spv::OpAccessChain || chain.operands.size() != 4 || operand != 2 || !chain.block ||
                    !entryFunctions.contains(chain.function)) { safe = false; break; }
                const auto* pointerType = definition(chain.type);
                if (!pointerType || pointerType->op != spv::OpTypePointer || pointerType->operands.size() != 3 ||
                    pointerType->operands[1] != spv::StorageClassWorkgroup || !unsigned32Type(pointerType->operands[2])) {
                    safe = false; break;
                }
                const auto pointerUses = uses(chain.result);
                if (pointerUses.empty()) { safe = false; break; }
                if (constantIs(chain.operands[3], slot)) {
                    bool recognized = false;
                    for (const auto& [id, protocol] : protocols) {
                        static_cast<void>(id);
                        if (protocol.base == base && protocol.pointer == chain.result) recognized = true;
                    }
                    if (recognized) continue;
                    if (pointerUses.size() != 1 || pointerUses[0].second != 0 || initializedFunction) { safe = false; break; }
                    const auto storeIndex = pointerUses[0].first;
                    const auto& store = instructions[storeIndex];
                    if (store.op != spv::OpStore || store.operands.size() != 2 || !constantIs(store.operands[1], 0) ||
                        store.block != entries.at(store.function) || chain.block != store.block ||
                        storeIndex != useIndex + 1 || storeIndex + 1 >= instructions.size()) { safe = false; break; }
                    const auto& barrier = instructions[storeIndex + 1];
                    if (barrier.block != store.block || barrier.op != spv::OpControlBarrier || barrier.operands.size() != 3 ||
                        !constantIs(barrier.operands[0], spv::ScopeWorkgroup) || !constantIs(barrier.operands[1], spv::ScopeWorkgroup) ||
                        !constantIs(barrier.operands[2], spv::MemorySemanticsAcquireReleaseMask | spv::MemorySemanticsWorkgroupMemoryMask)) {
                        safe = false; break;
                    }
                    initializedFunction = store.function;
                    initializerPointer = chain.result;
                    initializerBlock = store.block;
                    initializerStoreWordOffset = store.wordOffset;
                    initializerBarrierWordOffset = barrier.wordOffset;
                } else {
                    for (const auto& [memoryIndex, pointerOperand] : pointerUses) {
                        const auto& memory = instructions[memoryIndex];
                        const bool load = memory.op == spv::OpLoad && memory.operands.size() == 3 && pointerOperand == 2;
                        const bool store = memory.op == spv::OpStore && memory.operands.size() == 2 && pointerOperand == 0;
                        if ((!load && !store) || memory.function != chain.function ||
                            !boundedAccess(chain.operands[3], memory.block, slot)) { safe = false; break; }
                    }
                    if (!safe) break;
                }
            }
            if (!safe || !initializedFunction) continue;
            for (const auto id : selected) {
                const auto& protocol = protocols.at(id);
                const auto& compare = instructions[protocol.compare];
                if (compare.function != *initializedFunction || compare.block == entries.at(*initializedFunction)) safe = false;
            }
            if (safe) {
                electedLockCasResults.insert(selected.begin(), selected.end());
                for (const auto id : selected) {
                    auto metadata = protocols.at(id).metadata;
                    metadata.initializerPointer = initializerPointer;
                    metadata.initializerBlock = initializerBlock;
                    metadata.initializerStoreWordOffset = initializerStoreWordOffset;
                    metadata.initializerBarrierWordOffset = initializerBarrierWordOffset;
                    ldsLockProtocols.push_back(metadata);
                }
            }
        }
        std::sort(ldsLockProtocols.begin(), ldsLockProtocols.end(), [](const auto& left, const auto& right) {
            return left.casResult < right.casResult;
        });
    }
