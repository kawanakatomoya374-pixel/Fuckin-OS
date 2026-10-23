#include "../apps/module_interface.h"
#include <cstddef>

struct StorageSlot {
    char name[32]{};
    uint64_t capacity{};
    bool mounted{};
};

class StorageManagerCpp {
public:
    bool addSlot(const char* name, uint64_t capacity) {
        if (count_ >= MAX_SLOTS) return false;
        StorageSlot& slot = slots_[count_++];
        copyName(slot.name, name, sizeof(slot.name));
        slot.capacity = capacity;
        slot.mounted = true;
        return true;
    }

    uint64_t totalCapacity() const {
        uint64_t total = 0;
        for (int i = 0; i < count_; ++i) total += slots_[i].capacity;
        return total;
    }

    const StorageSlot* current() const {
        if (count_ == 0) return nullptr;
        return &slots_[current_];
    }

    void setCurrent(int idx) {
        if (idx >= 0 && idx < count_) current_ = idx;
    }

    void reset() {
        count_ = 0;
        current_ = 0;
        for (int i = 0; i < MAX_SLOTS; ++i) {
            slots_[i] = StorageSlot{};
        }
    }

private:
    static constexpr int MAX_SLOTS = 8;

    static void copyName(char* dst, const char* src, std::size_t size) {
        if (!dst || !size) return;
        std::size_t i = 0;
        if (!src) src = "";
        while (i + 1 < size && src[i]) { dst[i] = src[i]; ++i; }
        dst[i] = '\0';
    }

    StorageSlot slots_[MAX_SLOTS]{};
    int count_ = 0;
    int current_ = 0;
};

static StorageManagerCpp g_storage_cpp;

extern "C" {

void cpp_storage_manager_init(void) {
    g_storage_cpp.reset();
    g_storage_cpp.addSlot("RAM", 64ull * 1024ull * 1024ull);
}

uint64_t cpp_storage_total_capacity(void) {
    return g_storage_cpp.totalCapacity();
}

void cpp_storage_select(int idx) {
    g_storage_cpp.setCurrent(idx);
}

const char* cpp_storage_current_name(void) {
    const StorageSlot* slot = g_storage_cpp.current();
    return slot ? slot->name : "none";
}

}
