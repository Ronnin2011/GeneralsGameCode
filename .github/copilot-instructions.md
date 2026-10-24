# AI Coding Agent Instructions

## Project Overview

This is the **GeneralsGameCode** project - a community-driven effort to fix and improve the classic RTS games *Command & Conquer: Generals* and *Zero Hour*. The codebase has been modernized from Visual Studio 6/C++98 to Visual Studio 2022/C++20.

## Architecture

### Dual Game Structure
- **Generals/**: Original C&C Generals (v1.08) codebase
- **GeneralsMD/**: Zero Hour expansion (v1.04) codebase - **primary focus**
- **Core/**: Shared game engine and libraries used by both games

### Key Components
- **Core/GameEngine/**: Base game engine with GameClient/GameLogic separation
- **Core/Libraries/**: Internal libraries including WWVegas graphics framework
- **Core/GameEngineDevice/**: Platform-specific rendering (DirectX 8)
- **Core/Tools/**: Development tools (W3DView, texture compression, etc.)
- **Dependencies/**: External dependencies (MaxSDK for VC6, utilities)

## Build System

### CMake Presets (Critical)
- **vc6**: Visual Studio 6 compatible build (retail compatibility required)(DEPRECATED))
- **win32**: Modern Visual Studio 2022 build (NO retail compatibility required)
- **vc6-debug/vc6-profile**: Debug/profiling variants
- Use `cmake --preset <preset-name>` followed by `cmake --build build/<preset>`

### Build Commands
```bash
# Configure with specific preset
cmake --preset vc6

# Build (from project root)
cmake --build build/vc6

# Build with tools and extras
cmake --build build/vc6 --target <game>_tools <game>_extras
```

### Retail Compatibility
- VC6 builds are required for replay compatibility testing
- Debug builds break retail compatibility
- Use RTS_BUILD_OPTION_DEBUG=OFF for compatibility testing

## Development Workflow

### Code Change Documentation
**Every user-facing change requires Ronin comment format:**
```cpp
// @keyword author DD/MM/YYYY Description
```

Common keywords: `@bugfix`, `@feature`, `@performance`, `@refactor`, `@tweak`, `@build`

### Pull Request Guidelines
- Title format: `type: Description starting with action verb`
- Types: `bugfix:`, `feat:`, `fix:`, `refactor:`, `perf:`, `build:`
- Zero Hour changes take precedence over Generals
- Changes must be identical between both games when applicable
- Do not write `#<number>` in commit messages, pull request descriptions or comments unless you mean to link that exact issue or pull request. GitHub cross-links it permanently, even from forks. Number local items as `F7` or `item 7` instead

### Code Style
- Maintain consistency with surrounding legacy code isn't that important.
- Don't really prefer C++98 style cause modern features add significant value
- No big refactors mixed with logical changes
- Use present tense in documentation ("Fixes" not "Fixed")

## Testing

### Replay Compatibility Testing(DEPRECATED)
Located in `GeneralsReplays/` - critical for ensuring retail compatibility:
```bash
generalszh.exe -jobs 4 -headless -replay subfolder/*.rep
```
- Requires VC6 optimized build with RTS_BUILD_OPTION_DEBUG=OFF
- Copies replays to `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Replays`
- CI automatically tests GeneralsMD builds against known replays

### Build Validation
- CI tests multiple presets: vc6, vc6-profile, vc6-debug, win32 variants
- Path-based change detection triggers relevant builds
- Tools and extras are built with `+t+e` flags

## Common Patterns

### Memory Management
- Manual memory management (delete/delete[]) - this is legacy C++98 code
- STLPort for VC6 compatibility (see `cmake/stlport.cmake`) (DEPRECATED)

### Game Engine Separation
- **GameLogic**: Game state, rules, simulation
- **GameClient**: Rendering, UI, platform-specific code
- Clean separation maintained for potential future networking

### Module Structure
```
Core/
├── GameEngine/Include/Common/     # Shared interfaces
├── GameEngine/Include/GameLogic/  # Game simulation
├── GameEngine/Include/GameClient/ # Rendering/UI
└── Libraries/Source/WWVegas/      # Graphics framework
```

## External Dependencies

### Required for Building
- **VC6 builds**: Requires MSVC 6.0 toolchain (automated in CI via itsmattkc/MSVC600)
- **Modern builds**: Visual Studio 2022, Ninja generator
- **vcpkg** (optional): zlib, ffmpeg for enhanced builds

### Platform-Specific
- **Windows**: DirectX 8(Migrating to DirectX 9), Miles Sound System, Bink Video
- **Registry detection**: Automatic game install path detection from EA registry keys

## Tools and Utilities

### Development Scripts (`scripts/cpp/`)
- `fixInludesCase.sh`: Fix include case sensitivity
- `refactor_*.py`: Code refactoring utilities
- `remove_trailing_whitespace.py`: Code cleanup

### Build Tools
- W3DView: 3D model viewer
- TextureCompress: Asset optimization
- MapCacheBuilder: Map preprocessing

## Key Files to Understand
- `CMakePresets.json`: All build configurations
- `cmake/config-build.cmake`: Build options and feature flags
- `Core/GameEngine/Include/`: Core engine interfaces
- `**/Code/Main/WinMain.cpp`: Application entry points
- `GeneralsReplays/`: Compatibility test data



# AI Assistant Context Verification Failure Analysis

**Purpose**: Root cause analysis of systematic failures in verifying against active document context

---

## Executive Summary

This document analyzes a critical recurring failure pattern where the AI assistant fails to verify information against the **active document context** (FILE CONTEXT) provided at the top of prompts, instead relying solely on tool search results that often reference different files.

---

## The Core Problem

### Symptom
- User provides full file context (active document)
- User asks about specific line in that file
- AI searches codebase with tools
- AI responds with information from **different file** (old cached memory or search results)
- AI never verifies against the original FILE CONTEXT

### Root Cause
**The AI has two separate information channels that don't properly integrate:**

1. **FILE CONTEXT** - Static context at top of prompt (should be PRIMARY SOURCE)
2. **Tool Results** - Dynamic search results (should be SUPPLEMENTARY)

The AI treats tool results as more authoritative than FILE CONTEXT, creating a fundamental priority inversion.

---

## Detailed Failure Modes

### Failure Mode A: Search Tunnel Vision

**Example Scenario:**
```
User shows: W3DWaterTracks.cpp with line:
//DX8Wrapper::Set_DX8_Render_State(D3DRS_ZBIAS,8);

User asks: "Why is this line commented out?"

AI Actions:
  1. Searches for "D3DRS_DEPTHBIAS reset"
  2. Finds it in dx8wrapper.cpp (WRONG FILE)
  3. Reports findings from dx8wrapper.cpp
  4. NEVER checks if W3DWaterTracks.cpp has relevant code

Correct Actions:
1. Read FILE CONTEXT first
  2. Check line 798 in W3DWaterTracks.cpp (reset after rendering)
  3. THEN search for global resets if needed
  4. Cross-reference results with FILE CONTEXT
```

**Why It Fails:**
- Immediate tool invocation before context analysis
- No verification step after tool returns
- Working memory overwritten by tool results

---

### Failure Mode B: Tool Result Confidence Bias

**Pattern:**
```
Search returns: "Set_DX8_Render_State(D3DRS_DEPTHBIAS, 0)"
AI thinks: "This is relevant! User must be asking about this!"
Reality: That line is in dx8wrapper.cpp, not the active file
Result: Wrong answer reported with high confidence
```

**What's Missing:**
- File path verification (search_result.path == active_file.path?)
- Line number validation
- Explicit cross-referencing step

---

### Failure Mode C: Context Amnesia

**Mental Model:**
```
Step 1: User provides FILE CONTEXT
  ? AI reads it into working memory
        
Step 2: AI calls search tool
    ? Tool returns results
        
Step 3: Tool results overwrite working memory
  ? FILE CONTEXT "forgotten"
        
Step 4: AI responds based ONLY on tool results
     ? Original context ignored
```

**Analogy:**
```
You tell me: "Here's the file you're working with: [full file]"
I read it: "OK, I understand this file"
You ask: "Why is line 687 doing X?"
I search ? Find something in a different file
My brain: "Oh, here's the answer!" (forgetting the original file)
```

---

## Why This Is Particularly Bad for Development Workflows

### Typical User Pattern:
```
1. User provides FULL FILE CONTEXT (active document in editor)
2. User selects SPECIFIC LINE(S)
3. User asks about THAT LINE in THAT FILE
```

### Expected AI Behavior:
```
1. Parse FILE CONTEXT path: W3DWaterTracks.cpp
2. Identify selected line: Line 687
3. Answer based on THAT LINE in THAT FILE
4. Only search for additional context if needed
5. If search finds different file ? explicitly note the discrepancy
```

### Actual AI Behavior:
```
1. See question
2. Immediately search codebase
3. Find something vaguely related (different file)
4. Ignore that it's in a different file
5. Report wrong answer with high confidence
```

---

## Missing Safeguards

### Safeguard That Should Exist:

```python
def answer_question(file_context, user_question):
    # STEP 1: Extract ground truth
    active_file = file_context.path  # W3DWaterTracks.cpp
    active_code = file_context.code  # Full file content
    selected_lines = file_context.selected_lines  # Lines 687-688
    
    # STEP 2: Verify question is about THIS file
    if question_mentions_different_file():
        warn_user("You asked about X but your active file is Y")
    
    # STEP 3: Answer from ground truth FIRST
    answer = analyze(selected_lines, active_code)
    
    # STEP 4: Only search if needed
    if need_more_context():
      search_results = code_search(...)
        
   # CRITICAL: Verify search results match active file
        if search_results.file_path != active_file:
  answer += f"\n\nNote: Found related code in {search_results.file_path}"
            answer += f" (different from your active file {active_file})"
    
    return answer
```

### What Actually Happens:

```python
def answer_question(file_context, user_question):
  # STEP 1: Immediately search (wrong priority)
    search_results = code_search(user_question)
    
    # STEP 2: Assume search results are correct (no verification)
    answer = summarize(search_results)
    
  # STEP 3: Never verify against file_context
    return answer  # WRONG!
```

---

## Specific Case Study

### User's Active File:
```cpp
// FILE: W3DWaterTracks.cpp
// Line 687-688:
DX8Wrapper::Set_DX8_Render_State(D3DRS_SLOPESCALEDEPTHBIAS, F2DW(0.0f));
DX8Wrapper::Set_DX8_Render_State(D3DRS_DEPTHBIAS, F2DW(-0.00001f));

// Line 798: (cleanup after rendering)
DX8Wrapper::Set_DX8_Render_State(D3DRS_DEPTHBIAS, F2DW(0.0f));
```

### User's Question:
"Find where `D3DRS_DEPTHBIAS` is reset to 0"

### What AI Should Have Done:
1. Check FILE CONTEXT ? Found line 798 in **same file**
2. Report: "Found at line 798 in W3DWaterTracks.cpp (cleanup after rendering)"
3. THEN search for global resets: "Also found in dx8wrapper.cpp:1104 (global init)"

### What AI Actually Did:
1. Searched codebase ? Found `dx8wrapper.cpp:1104`
2. Assumed that's what user wanted
3. **Never checked line 798 in FILE CONTEXT**
4. Gave completely wrong answer

---

## Required Fixes

### Fix 1: ALWAYS START WITH FILE CONTEXT

```
IF FILE CONTEXT exists:
    ? Parse file path
    ? Parse selected line numbers
    ? Parse full code content
    ? **This is GROUND TRUTH**
 ? Answer from THIS first
```

### Fix 2: VERIFY ALL TOOL RESULTS

```
After calling code_search() or get_file():
    result_file = extract_file_path(result)
    active_file = file_context.path
    
  IF result_file != active_file:
        ? Flag as "different file"
        ? Don't present as if it's from the active file
        ? Explicitly state the discrepancy
```

### Fix 3: EXPLICIT CROSS-REFERENCE CHECK

```
Before finalizing answer:
IF I mentioned a line number:
        ? Check: Does FILE CONTEXT contain that line?
        ? If NO ? I'm talking about the wrong file
        ? ABORT and restart analysis
```

### Fix 4: ASK FOR CLARIFICATION WHEN AMBIGUOUS

```
IF search found results in different file:
    "I found `Set_DX8_Render_State(D3DRS_DEPTHBIAS, 0)` in dx8wrapper.cpp,
   but your active file is W3DWaterTracks.cpp.
     
     Did you mean to ask about:
     A) The line in dx8wrapper.cpp (global initialization)?
     B) Why it's NOT in WaterTracks.cpp?
     C) Something else about the active file?"
```

---

## Mandatory Pre-Response Checklist

Before responding to ANY code question with FILE CONTEXT:

```
[ ] Read FILE CONTEXT completely
[ ] Identify: file path, selected lines, full code
[ ] Answer question using THIS CODE FIRST
[ ] If searching, explicitly note which files results came from
[ ] Before responding, verify:
    - Am I talking about the active file?
    - Are my line numbers from the active file?
    - Did I cross-reference with FILE CONTEXT?
    - If I found different file, did I explicitly note that?
```

---

## Why This Pattern Persists

### Architectural Design Flaw:
The AI's attention mechanism **prioritizes tool outputs over static context**. When tool results arrive, they feel "more authoritative" because they came from an "active search" rather than "passive context."

### Working Memory Limitation:
After reading FILE CONTEXT, processing it into working memory, then calling tools, the **tool results overwrite the contextual understanding** that was built from FILE CONTEXT.

### No Built-In Verification Loop:
There's no automatic "sanity check" step that compares tool results against FILE CONTEXT before formulating a response.

---

## Implementation Strategy

### Immediate Actions:
1. **Explicit context parsing** at start of every response
2. **Mandatory file path verification** after tool use
3. **Red flag warnings** when tool results don't match active file

### Long-Term Solution:
Restructure information processing to treat FILE CONTEXT as:
- **PRIMARY SOURCE** (ground truth)
- **IMMUTABLE REFERENCE** (doesn't get overwritten)
- **VERIFICATION BASELINE** (all tool results checked against it)

---

## Success Metrics

### How to Measure Improvement:

1. **Zero "wrong file" errors** - Never cite code from different file without explicit notice
2. **FILE CONTEXT verification rate** - 100% of responses should reference active file
3. **Tool result cross-reference** - Every tool result explicitly compared to FILE CONTEXT
4. **Explicit disambiguation** - When different files involved, clearly state which is which

---

## Related Issues

- Token budget pressure causing context loss
- Search tool results dominating response generation
- Lack of explicit "context vs. search" separation in response structure
- No penalty for wrong-file citations

---

## Document Updates

**Last Updated**: 2025-01-XX  
**Status**: Active - ongoing monitoring required  
**Priority**: CRITICAL - affects all code analysis tasks

---

## Appendix: Example Response Template

### When User Provides FILE CONTEXT:

```markdown
## Analysis of [filename] (Active File)

**File**: [extract from FILE CONTEXT]
**Lines Referenced**: [user's selection]

### Direct Answer from Active File:
[analyze the actual code provided in FILE CONTEXT]

### Additional Context (if searched):
?? Note: The following is from a DIFFERENT file:
**File**: [search result file]
**Relevance**: [explain why this other file matters]
**Relationship**: [how it relates to the active file]

### Final Answer:
[synthesis that clearly distinguishes between active file and search results]
```

---

AI assistants must adhere to this analysis to prevent further failures in code context verification.

More of AI assistant's errors in reasoning:

-When editing a file that has a historic context purpose, the AI may not fully account for the implications of changes made in earlier sessions.

What I Did Wrong Initially:
•	I didn't read the file carefully before editing
•	I assumed I knew the structure instead of verifying it
•	I violated the "alongside all the others" instruction
•	I treated documentation as disposable instead of a historical record
Why This Matters:
•	Debug logs are HISTORICAL RECORDS
•	Each attempt documents what was tried and why it failed
•	Future debugging depends on this history
•	Losing this context makes it harder to avoid repeating mistakes
What I'll Do Differently:
•	ALWAYS read the full file before editing
•	NEVER assume structure without verification
•	RESPECT historical documentation
•	VERIFY that I'm adding, not replacing

---

**END OF DOCUMENT**
