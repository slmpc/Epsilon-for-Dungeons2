<#
.SYNOPSIS
    Audit a source tree for leftover snake_case identifiers after the camelCase migration.

.DESCRIPTION
    Reports identifiers that still contain an underscore, excluding:
      * trailing-underscore data members (className_), which the migration kept on purpose
      * the standard library / Windows SDK / ImGui names the migration must not touch
      * header guards, include paths and macro-only names

    Any hit is either a genuine miss or needs an explicit allow-list entry here.
#>
[CmdletBinding()]
param(
    [string[]]$Paths = @('src', 'tests'),
    [switch]$ShowContext
)

$ErrorActionPreference = 'Stop'

# Names belonging to the C++ standard library, Win32 SDK, ImGui, MinHook or vcpkg
# ports. These keep their own casing and are excluded from the audit.
$allowed = @(
    '^_', '^__', '^_CRT', '^_WIN', '^_UNICODE$', '^_beginthreadex$', '^_read$',
    '^WIN32_LEAN_AND_MEAN$', '^PAGE_', '^MEM_', '^FILE_', '^GENERIC_', '^CREATE_', '^OPEN_',
    '^INVALID_HANDLE_VALUE$', '^ERROR_', '^WAIT_', '^PROCESS_', '^THREAD_', '^STATUS_',
    '^STD_', '^CP_UTF8$', '^ENABLE_', '^D3D', '^DXGI_', '^DXGI$', '^IID_', '^WNDCLASS',
    '^IDC_', '^IDI_', '^CFG_', '^SW_', '^CS_', '^WS_', '^PM_', '^WM_', '^VK_', '^MF_',
    '^DISP_', '^SEC_', '^SE_', '^EXCEPTION_', '^CONTEXT_', '^IMAGE_', '^IMGUI_', '^ImGui',
    '^MINIDUMP', '^MH_', '^MH$', '^VCPKG_', '^CMAKE_', '^NOMINMAX$', '^MB_',
    '^FORCEINLINE$', '^S_OK$', '^E_', '^LPVOID$', '^LP', '^HWND$', '^HANDLE$', '^HMODULE$',
    '^DWORD$', '^WORD$', '^BOOL$', '^LONG$', '^ULONG', '^UINT', '^INT', '^CHAR', '^WCHAR',
    '^TCHAR', '^BYTE$', '^FLOAT$', '^VOID$', '^SIZE_T$', '^SSIZE_T$', '^ULONGLONG$',
    '^LARGE_INTEGER$', '^MEMORY_BASIC_INFORMATION$', '^MODULEINFO$', '^PROCESSENTRY32',
    '^MODULEENTRY32', '^THREADENTRY32$', '^OSVERSIONINFO', '^OVERLAPPED$',
    '^TOKEN_', '^KEY_', '^HKEY_', '^REG_', '^FORMAT_', '^MAKELANGID', '^SUBLANG_',
    '^GET_X', '^GET_Y', '^LOWORD$', '^HIWORD$', '^MAKELONG$', '^RGB$', '^MAX_PATH$',
    '^TRUE$', '^FALSE$', '^NULL$', '^PIPE_', '^PIPE$', '^BUFSIZ$', '^SEEK_', '^PRI',
    '^INT_', '^UINT_', '^FLT_', '^DBL_', '^SIG', '^EAI_', '^AI_', '^AF_', '^SOCK_',
    '^IPPROTO_', '^SO_', '^INADDR_', '^hton', '^ntoh', '^S_', '^O_', '^F_', '^RTL_',
    '^UNW_', '^CONTEXT$', '^EXCEPTION_RECORD$', '^PEXCEPTION', '^va_', '^assert$',
    '^CW_USEDEFAULT$', '^PFN_', '^CreateDXGIFactory', '^Error$', '^TYPE$',
    # standard-language / STL member and cast spellings
    '^[a-z_]*_cast$', '^c_str$', '^pop_back$', '^push_back$', '^static_assert$',
    '^string_view$', '^wstring_view$', '^unique_ptr$', '^shared_ptr$', '^make_unique$',
    '^lock_guard$', '^memory_order_', '^for_each$', '^size_t$', '^ptrdiff_t$',
    '^int8_t$', '^int16_t$', '^int32_t$', '^int64_t$', '^uint8_t$', '^uint16_t$',
    '^uint32_t$', '^uint64_t$', '^uintptr_t$', '^intptr_t$', '^wchar_t$', '^char16_t$',
    '^to_string$', '^starts_with$', '^ends_with$', '^find_last_of$', '^find_first_of$',
    # nlohmann/json 的谓词与访问器(第三方 API, 必须保持原样)
    '^is_null$', '^is_boolean$', '^is_number$', '^is_string$', '^is_array$', '^is_object$',
    '^is_number_integer$', '^is_number_float$', '^is_discarded$', '^is_primitive$',
    '^is_structured$', '^error_handler_t$', '^get_impl$', '^from_json$', '^to_json$',
    # 注释里出现的类名/库名, 不是标识符
    '^magic_enum$', '^auto_sprint$',
    # std::filesystem 谓词与访问器
    '^is_directory$', '^is_regular_file$', '^is_symlink$', '^is_empty$',
    '^directory_iterator$', '^recursive_directory_iterator$', '^remove_all$',
    '^create_directories$', '^current_path$', '^temp_directory_path$',
    # unordered_map / 其它 STL 类型
    '^unordered_map$', '^unordered_set$', '^deque$', '^optional$',
    # more Win32 SDK spellings
    '^GWLP_', '^GWL_', '^LONG_PTR$', '^GET_MODULE_HANDLE_EX_FLAG', '^ACCESS_DENIED$',
    '^CW_', '^CS_', '^IDC_', '^SW_', '^OFN_', '^RPC_', '^SECURITY_', '^SE_',
    # D3D12 / DXGI / ImGui / MinHook API spellings
    '^D3D12_', '^DXGI_', '^dxgi1_', '^IM_', '^ImGui', '^MH_', '^DLL_PROCESS_',
    '^EXECUTE_READ', '^RENDER_TARGET', '^SHADER_VISIBLE', '^value_or$', '^fetch_add$',
    # UE engine field names quoted in comments: they are the engine's own spelling,
    # not ours, so they must not be camel-cased.
    '^Offset_Internal$',
    '^OnRep_', '^PlayerCharacter_C$', '^MovementSpeedMultiplier$',
    # nlohmann 的 optional 访问器 / Win32 标志
    '^has_value$', '^MOVEFILE_',
    # project-owned macros that intentionally keep SCREAMING_SNAKE_CASE
    '^EPSILON_PIPE_NAME$', '^EPSILON_TARGET_NO_CLEAR$', '^NOMINMAX$', '^_t$'
)

$files = Get-ChildItem -Path $Paths -Recurse -File -Include *.h, *.hpp, *.cpp, *.cxx |
    Where-Object { $_.FullName -notmatch '\\vcpkg_installed\\|\\build\\' }

$found = 0
foreach ($f in $files) {
    $lines = Get-Content $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        # skip preprocessor lines and pure comments to cut noise
        if ($line -match '^\s*#') { continue }
        # A token that directly follows "::" belongs to a library namespace
        # (std::string_view, ImGui::Text, ...) and must keep its own spelling.
        # Record those positions instead of rewriting the line, so that later
        # qualified names on the same line stay qualified.
        $qualified = @{}
        foreach ($q in [regex]::Matches($line, '::\s*([A-Za-z_]\w*)')) {
            $qualified[$q.Groups[1].Index] = $true
        }
        foreach ($m in [regex]::Matches($line, '(?<![\w])([A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]*)(?![\w])')) {
            $tok = $m.Groups[1].Value
            if ($qualified.ContainsKey($m.Groups[1].Index)) { continue }
            if ($tok -match '_$') { continue }              # kept member convention
            $skip = $false
            foreach ($a in $allowed) { if ($tok -match $a) { $skip = $true; break } }
            if ($skip) { continue }
            $rel = $f.FullName.Replace((Get-Location).Path + '\', '')
            if ($ShowContext) {
                "{0}:{1}: {2}" -f $rel, ($i + 1), $line.Trim()
            } else {
                "{0}:{1}: {2}" -f $rel, ($i + 1), $tok
            }
            $found++
        }
    }
}
Write-Host "`n=== residual snake_case identifiers: $found ==="
