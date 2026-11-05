#include "model.h"
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace zen;
static_assert(!std::is_convertible_v<LocationKey, std::wstring>);
static_assert(!std::is_convertible_v<LocationKey, const wchar_t *>);
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition))                                                                                    \
            throw std::runtime_error(#condition);                                                            \
    } while (false)

static void LocationIdentity() {
    LocationStore locations;
    auto item = ParseItem(Home);
    CHECK(item);
    auto home = locations.Remember(item.Get());
    CHECK(home == Home);
    LocationKey alias = L"zen-shell:no-parsing-name";
    auto record = *locations.Find(home);
    record.parsingName.clear();
    locations.Set(alias, record);
    CHECK(locations.FilePath(alias).empty());
    CHECK(locations.Text(alias) == record.displayName);
    LocationResolution resolved;
    CHECK(SUCCEEDED(locations.Resolve(alias, resolved)) && resolved.savedIdentity);
    ComPtr<IShellItem> restored;
    CHECK(SUCCEEDED(locations.ResolveItem(alias, &restored)) && ItemName(restored.Get()) == Home);
    locations.Invalidate(alias);
    CHECK(FAILED(locations.Resolve(alias, resolved)));
    locations.AllowRetry(alias);
    CHECK(SUCCEEDED(locations.Resolve(alias, resolved)) && resolved.savedIdentity);
    record.parsingName = Home;
    locations.Set(alias, record);
    locations.Invalidate(alias);
    CHECK(SUCCEEDED(locations.Resolve(alias, resolved)) && !resolved.savedIdentity);
    locations.Retain({alias});
    CHECK(locations.Find(alias) && !locations.Find(home));
    CHECK(locations.FilePath(L"C:\\work") == L"C:\\work");
}

static void NavigationTransitions() {
    LocationStore locations;
    auto item = ParseItem(Home);
    CHECK(item);
    auto home = locations.Remember(item.Get());
    LocationResolution saved;
    CHECK(SUCCEEDED(locations.Resolve(home, saved)));
    auto pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(saved.pidl.data());
    USHORT desktopEnd = 0;
    auto desktop = reinterpret_cast<PCIDLIST_ABSOLUTE>(&desktopEnd);
    NavigationState state;
    state.Begin({home, false, 2});
    state.SetIdentity(saved);
    CHECK(state.AcceptsCompletion(pidl) && !state.AcceptsCompletion(desktop));
    CHECK(state.Fail(E_FAIL));
    CHECK(!state.Pending() && state.Error() == E_FAIL && !state.AcceptsCompletion(pidl));
    CHECK(state.Request().destination == home && state.Request().historyIndex == 2);
    auto retry = state.Request();
    state.Begin(retry);
    saved.savedIdentity = false;
    state.SetIdentity(saved);
    CHECK(!state.Fail(E_FAIL)); // 系统名称回退失败不能再次自动回退。
    state.Begin(retry);
    saved.savedIdentity = true;
    state.SetIdentity(saved);
    CHECK(!state.Fail(E_ABORT));
    CHECK(!state.AcceptsCompletion(pidl)); // 取消后的迟到结果不能提交。
    state.Begin({L"desktop"});
    state.SetIdentity({{0, 0}, false});
    CHECK(!state.AcceptsCompletion(pidl) && state.AcceptsCompletion(desktop));
    state.Complete();
    CHECK(!state.Pending() && state.Request().destination.empty() && state.Error() == S_OK);
}

static void NavigationCommit() {
    Model model;
    auto id = model.panes[0].selected;
    auto *tab = model.Find(id);
    tab->viewMode = FVM_ICON;
    tab->iconSize = 48;
    model.CommitNavigation(id, L"zen-shell:normalized", {Home, false}, true);
    CHECK(tab->path == L"zen-shell:normalized" && tab->history.size() == 1);
    CHECK(tab->history[0] == tab->path && tab->viewMode == FVM_ICON && tab->iconSize == 48);
    model.CommitNavigation(id, L"C:\\work", {L"C:\\work"}, false);
    CHECK(tab->viewMode == FVM_DETAILS && tab->historyIndex == 1);
    auto history = tab->history;
    NavigationState request;
    request.Begin({history[0], false, 0});
    request.Fail(E_FAIL);
    CHECK(tab->historyIndex == 1 && tab->history == history && tab->path == L"C:\\work");
    model.CommitNavigation(id, history[0], request.Request(), false);
    CHECK(tab->historyIndex == 0 && tab->path == history[0] && tab->history == history);
    CHECK(!model.CommitNavigation(id, L"C:\\invalid", {L"C:\\invalid", false, 999}));
    CHECK(tab->historyIndex == 0 && tab->path == history[0] && tab->history == history);
}

int main() {
    auto root = std::filesystem::current_path() / L".test-data" /
                (L"navigation-" + std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"ZENEXPLORER_ROOT_DIR", root.c_str());
    SetEnvironmentVariableW(L"ZENEXPLORER_FILE_LOG", nullptr);
    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr))
        return 1;
    bool failed = false;
    struct Case {
        const char *name;
        void (*run)();
    };
    for (const auto &test :
         {Case{"location_identity", LocationIdentity}, Case{"navigation_transitions", NavigationTransitions},
          Case{"navigation_commit", NavigationCommit}}) {
        try {
            test.run();
            std::cout << test.name << ": passed\n";
        } catch (const std::exception &error) {
            std::cerr << test.name << ": " << error.what() << '\n';
            failed = true;
        }
    }
    OleUninitialize();
    return failed ? 1 : 0;
}
