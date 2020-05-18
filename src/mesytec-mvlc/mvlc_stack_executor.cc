#include "mvlc_stack_executor.h"
#include "mvlc_constants.h"
#include "mvlc_readout_parser.h"
#include "vme_constants.h"
#include <bits/c++config.h>

namespace mesytec
{
namespace mvlc
{
namespace detail
{

 std::vector<std::vector<StackCommand>> split_commands(
    const std::vector<StackCommand> &commands,
    const CommandExecOptions &options,
    const u16 immediateStackMaxSize)
{
    std::vector<std::vector<StackCommand>> result;

    if (options.noBatching)
    {
        for (const auto &cmd: commands)
            result.push_back({ cmd });

        return result;
    }

    auto first = std::begin(commands);
    const auto end = std::end(commands);

    while (first < end)
    {
        size_t encodedSize = 2u;

        auto partEnd = std::find_if(
            first, end, [&] (const StackCommand &cmd)
            {
                if (is_sw_delay(cmd) && !options.ignoreDelays)
                    return true;

                if (encodedSize + get_encoded_size(cmd) > immediateStackMaxSize)
                    return true;

                encodedSize += get_encoded_size(cmd);

                return false;
            });

        if (first < end && first == partEnd && is_sw_delay(*first))
            ++partEnd;

        if (first == partEnd)
            throw std::runtime_error("split_commands: not advancing");

        std::vector<StackCommand> part;
        part.reserve(partEnd - first);
        std::copy(first, partEnd, std::back_inserter(part));
        result.emplace_back(part);
        first = partEnd;
    }

    return result;
}

using FrameParseState = readout_parser::ReadoutParserState::FrameParseState;

struct ParseState
{
    Result result;
    FrameParseState curBlockFrame;
};

template<typename Iter>
Iter parse_stack_frame(
    basic_string_view<const u32> stackFrame,
    Iter itCmd, const Iter cmdsEnd,
    ParseState &state,
    std::vector<Result> &dest)
{
    using CT = StackCommand::CommandType;

    while (itCmd != cmdsEnd)
    {
        switch (itCmd->type)
        {
            case CT::Invalid:
                throw std::runtime_error("Invalid stack command type");

            case CT::StackStart:
            case CT::StackEnd:
                ++itCmd;
                break;

            case CT::SoftwareDelay:
                state.result = Result(*itCmd);
                dest.emplace_back(state.result);
                state.result.clear();
                ++itCmd;
                break;

            case CT::VMERead:
                if (stackFrame.size() == 0)
                    return itCmd;

                if (!vme_amods::is_block_mode(itCmd->amod))
                {
                    u32 value = stackFrame[0];

                    if (itCmd->dataWidth == VMEDataWidth::D16)
                        value &= 0xffffu;

                    state.result = Result(*itCmd);
                    state.result.response = { value };
                    stackFrame.remove_prefix(1);
                    dest.emplace_back(state.result);
                    state.result.clear();
                    ++itCmd;
                    break;
                }
                else
                {
                    if (!state.result.valid())
                    {
                        state.result = Result(*itCmd);

                        if (!is_blockread_buffer(stackFrame[0]))
                            throw std::runtime_error("parse_stack_frame: expected BlockRead frame");

                        state.curBlockFrame = FrameParseState(stackFrame[0]);
                        stackFrame.remove_prefix(1);
                    }

                    assert(state.result.valid());
                    assert(vme_amods::is_block_mode(state.result.cmd.amod));

                    while (true)
                    {
                        if (state.curBlockFrame.wordsLeft == 0)
                        {
                            if (state.curBlockFrame.info().flags & frame_flags::Continue)
                            {
                                if (stackFrame.empty())
                                    return itCmd;

                                if (!is_blockread_buffer(stackFrame[0]))
                                    throw std::runtime_error("parse_stack_frame: expected BlockRead frame");

                                state.curBlockFrame = FrameParseState(stackFrame[0]);
                                stackFrame.remove_prefix(1);
                            }
                            else
                            {
                                dest.emplace_back(state.result);
                                state.result.clear();
                                ++itCmd;
                                break;
                            }
                        }

                        size_t toCopy = std::min(
                            static_cast<size_t>(state.curBlockFrame.wordsLeft),
                            stackFrame.size());

                        if (toCopy == 0)
                            break;

                        std::copy(std::begin(stackFrame), std::begin(stackFrame) + toCopy,
                                  std::back_inserter(state.result.response));

                        state.curBlockFrame.consumeWords(toCopy);

                        stackFrame.remove_prefix(toCopy);
                    }
                }
                break;

            case CT::VMEWrite:
                state.result = Result(*itCmd);
                dest.emplace_back(state.result);
                state.result.clear();
                ++itCmd;
                break;

            case CT::WriteMarker:
            case CT::WriteSpecial:
                state.result = Result(*itCmd);
                state.result.response = { stackFrame[0] };
                stackFrame.remove_prefix(1);
                dest.emplace_back(state.result);
                state.result.clear();
                ++itCmd;
                break;
        }
    }

    return itCmd;
}

} // end namespace detail

std::vector<Result> parse_response_list(
    const std::vector<StackCommand> &commands, const std::vector<u32> &responseBuffer)
{
    if (commands.empty())
        return {};

    auto itCmd = std::begin(commands);
    auto cmdsEnd = std::end(commands);

    basic_string_view<const u32> response(responseBuffer.data(), responseBuffer.size());

    if (response.empty())
        throw std::runtime_error("parse_response: empty response buffer");

    detail::ParseState parseState;
    std::vector<Result> results;

    while (!response.empty() && itCmd != cmdsEnd)
    {
        u32 stackFrameHeader = response[0];

        if (!is_stack_buffer(stackFrameHeader))
            throw std::runtime_error("parse_response: expected StackFrame header");

        auto frameInfo = extract_frame_info(stackFrameHeader);

        if (response.size() < frameInfo.len + 1u)
            throw std::runtime_error("parse_response: StackFrame length exceeds response size");

        response.remove_prefix(1);

        basic_string_view<const u32> stackFrame(response.data(), frameInfo.len);

        itCmd = parse_stack_frame(
            stackFrame, itCmd, cmdsEnd,
            parseState, results);

        response.remove_prefix(frameInfo.len);

        while (frameInfo.flags & frame_flags::Continue)
        {
            stackFrameHeader = response[0];

            if (!is_stack_buffer_continuation(stackFrameHeader))
                throw std::runtime_error("parse_response: expected StackContinuation header");

            frameInfo = extract_frame_info(stackFrameHeader);

            if (response.size() < frameInfo.len + 1u)
                throw std::runtime_error("parse_response: StackContinuation length exceeds response size");

            response.remove_prefix(1);

            basic_string_view<const u32> stackFrame(response.data(), frameInfo.len);

            itCmd = parse_stack_frame(
                stackFrame, itCmd, cmdsEnd,
                parseState, results);

            response.remove_prefix(frameInfo.len);
        }
    }

    return results;
}

GroupedStackResults MESYTEC_MVLC_EXPORT parse_stack_exec_response(
    const StackCommandBuilder &stack,
    const std::vector<u32> &responseBuffer,
    const std::vector<std::error_code> &execErrors)
{
    auto results = parse_response_list(stack.getCommands(), responseBuffer);

    GroupedStackResults ret;

    auto stackGroups  = stack.getGroups();
    auto itResults = std::begin(results);
    const auto endResults = std::end(results);
    auto itErrors = execErrors.begin();
    const auto endErrors = std::end(execErrors);

    for (const auto &stackGroup: stackGroups)
    {
        GroupedStackResults::Group resultsGroup;
        resultsGroup.name = stackGroup.name;

        for (size_t i = 0; i < stackGroup.commands.size(); ++i)
        {
            if (itResults >= endResults)
                break;

            if (itErrors < endErrors)
                itResults->ec = *itErrors;

            resultsGroup.results.push_back(*itResults++);
        }

        ret.groups.emplace_back(resultsGroup);
        ++itErrors;
    }

    return ret;
}

} // end namespace mvlc
} // end namespace mesytec
