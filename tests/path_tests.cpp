#include "../src/common/paths.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>

struct TestCase {
    const wchar_t* input;
    bool shouldPass;
    const char* description;
};

int main() {
    wchar_t root[MAX_PATH];
    GetFullPathNameW(L".", MAX_PATH, root, nullptr);

    printf("=== PATH VALIDATION TESTS ===\n");
    printf("Root directory: %ls\n\n", root);

    TestCase tests[] = {
        // MUST REJECT:
        { L"..\\x", false, "Traversal leading .." },
        { L"a\\..\\..\\x", false, "Traversal nested double .." },
        { L"a\\..\\b", false, "Traversal intermediate .." },
        { L"..", false, "Bare .." },
        { L"C:\\Windows\\x", false, "Absolute drive letter path" },
        { L"C:x", false, "Drive-relative path" },
        { L"\\\\srv\\share\\x", false, "UNC path" },
        { L"\\\\?\\C:\\x", false, "Extended-length NT path prefix" },
        { L"\\x", false, "Root-relative path with backslash" },
        { L"/x", false, "Root-relative path with forward slash" },
        { L"file.txt:stream", false, "Alternate Data Stream colon" },
        { L"CON", false, "DOS device CON" },
        { L"nul.txt", false, "DOS device nul with extension" },
        { L"a.", false, "Segment ending in dot" },
        { L"a ", false, "Segment ending in space" },
        { L"", false, "Empty path" },

        // MUST ACCEPT:
        { L"scripts\\a.asi", true, "Standard relative ASI path" },
        { L"data/maps/x.dat", true, "Forward slashes normalized" },
        { L"modloader\\Моя папка\\файл.txt", true, "Cyrillic unicode folder and file" },
        { L"a b\\c.txt", true, "Folder with spaces" }
    };

    int passed = 0;
    int failed = 0;

    wchar_t out[MAX_PATH];
    for (const auto& tc : tests) {
        PathError err = PathError::Ok;
        bool ok = ResolveInsideRoot(root, tc.input, out, MAX_PATH, &err);

        bool testPassed = (ok == tc.shouldPass);
        if (testPassed) {
            passed++;
            printf("[PASS] %-35s -> %s (Err: %s)\n",
                   tc.description, ok ? "ACCEPTED" : "REJECTED", PathErrorToString(err));
        } else {
            failed++;
            printf("[FAIL] %-35s -> Expected %s, got %s (Err: %s)\n",
                   tc.description, tc.shouldPass ? "ACCEPT" : "REJECT", ok ? "ACCEPT" : "REJECT", PathErrorToString(err));
        }
    }

    // Protected path tests
    printf("\n=== PROTECTED PATHS TESTS ===\n");
    const wchar_t* protectedItems[] = {
        L"gta-vc.exe",
        L"updater.exe",
        L"UpdateChecker.asi",
        L"dinput8.dll",
        L"updater.ini",
        L"installed_manifest.txt",
        L".updaterbackups",
        L"scripts",
        L"modloader"
    };

    for (const auto& p : protectedItems) {
        bool prot = IsProtectedPath(root, p);
        if (prot) {
            passed++;
            printf("[PASS] Protected check: %ls is PROTECTED\n", p);
        } else {
            failed++;
            printf("[FAIL] Protected check: %ls was NOT protected!\n", p);
        }
    }

    const wchar_t* nonProtectedItems[] = {
        L"scripts\\my_custom_mod.asi",
        L"modloader\\new_skin\\skin.txd"
    };

    for (const auto& np : nonProtectedItems) {
        bool prot = IsProtectedPath(root, np);
        if (!prot) {
            passed++;
            printf("[PASS] Non-protected check: %ls is MODIFIABLE\n", np);
        } else {
            failed++;
            printf("[FAIL] Non-protected check: %ls was marked PROTECTED unexpectedly!\n", np);
        }
    }

    printf("\nTotal: %d Passed, %d Failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
