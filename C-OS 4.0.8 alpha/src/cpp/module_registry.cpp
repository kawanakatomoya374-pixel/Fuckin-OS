#include "../apps/module_interface.h"
#include <cstddef>

class ModuleRegistry {
public:
    bool registerModule(module_interface_t* module) {
        if (!module || !module->name) return false;
        if (count_ >= MAX_MODULES) return false;
        for (int i = 0; i < count_; ++i) {
            if (modules_[i] && same(modules_[i]->name, module->name)) return false;
        }
        modules_[count_++] = module;
        return true;
    }

    bool unregisterModule(const char* name) {
        if (!name) return false;
        for (int i = 0; i < count_; ++i) {
            if (modules_[i] && same(modules_[i]->name, name)) {
                if (modules_[i]->cleanup) modules_[i]->cleanup();
                for (int j = i; j < count_ - 1; ++j) modules_[j] = modules_[j + 1];
                modules_[--count_] = nullptr;
                return true;
            }
        }
        return false;
    }

    module_interface_t* find(const char* name) const {
        if (!name) return nullptr;
        for (int i = 0; i < count_; ++i) {
            if (modules_[i] && same(modules_[i]->name, name)) return modules_[i];
        }
        return nullptr;
    }

    void initAll() {
        for (int i = 0; i < count_; ++i) {
            if (modules_[i] && modules_[i]->init) {
                modules_[i]->status = static_cast<uint8_t>(modules_[i]->init() == MODULE_STATUS_OK ? MODULE_STATUS_OK : MODULE_STATUS_ERROR);
            }
        }
    }

    void cleanupAll() {
        for (int i = count_ - 1; i >= 0; --i) {
            if (modules_[i] && modules_[i]->cleanup) modules_[i]->cleanup();
            modules_[i] = nullptr;
        }
        count_ = 0;
    }

    int activeCount() const {
        int active = 0;
        for (int i = 0; i < count_; ++i) {
            if (modules_[i] && modules_[i]->status == MODULE_STATUS_OK) ++active;
        }
        return active;
    }

private:
    static bool same(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b) {
            if (*a != *b) return false;
            ++a; ++b;
        }
        return *a == *b;
    }

    module_interface_t* modules_[MAX_MODULES]{};
    int count_ = 0;
};

static ModuleRegistry g_registry;

extern "C" {

int cpp_module_register(module_interface_t* module) {
    return g_registry.registerModule(module) ? MODULE_STATUS_OK : MODULE_STATUS_ERROR;
}

int cpp_module_unregister(const char* name) {
    return g_registry.unregisterModule(name) ? MODULE_STATUS_OK : MODULE_STATUS_ERROR;
}

module_interface_t* cpp_module_find(const char* name) {
    return g_registry.find(name);
}

void cpp_module_init_all(void) {
    g_registry.initAll();
}

void cpp_module_cleanup_all(void) {
    g_registry.cleanupAll();
}

int cpp_module_active_count(void) {
    return g_registry.activeCount();
}

}
