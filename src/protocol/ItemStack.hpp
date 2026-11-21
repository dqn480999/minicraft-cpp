#pragma once

#include "common/Types.hpp"

namespace mc {

class ByteBuffer;

struct ItemStack {
    i16 id{-1};
    i8 count{0};
    i16 damage{0};

    bool empty() const {
        return id < 0 || count <= 0;
    }
};

int maxStackSize(i16 itemId);
bool isValidItemId(i16 itemId);
ItemStack sanitizeItemStack(ItemStack stack);
void writeItemStack(ByteBuffer& buffer, const ItemStack& stack);
ItemStack readItemStack(ByteBuffer& buffer);

}
