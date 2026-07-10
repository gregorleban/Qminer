#include <base.h>
#include <mine.h>
#include "gtest/gtest.h"

#ifdef GLib_CPP11

TEST(Traits, is_containerTVec) {
    ASSERT_TRUE(gtraits::is_container<TIntV>::value);
    ASSERT_TRUE(gtraits::is_container<TStrV>::value);
    ASSERT_TRUE(gtraits::is_container<TStrPrV>::value);
}

TEST(Traits, is_containerTHash) {
    ASSERT_TRUE(gtraits::is_container<TIntH>::value);
    ASSERT_TRUE(gtraits::is_container<TIntStrH>::value);
    ASSERT_TRUE(gtraits::is_container<TIntFltH>::value);
}

// test that glib numeric types have standard layouts
TEST(Traits, type_traitsTNum) {
    ASSERT_TRUE(gtraits::is_shallow<TInt>::value);
    ASSERT_TRUE(gtraits::is_shallow<TUInt>::value);
    ASSERT_TRUE(gtraits::is_shallow<TFlt>::value);
    ASSERT_TRUE(gtraits::is_shallow<TUInt64>::value);
    ASSERT_TRUE(gtraits::is_shallow<TUSInt>::value);
}

// characters
TEST(Traits, type_traitsTCh) {
    ASSERT_TRUE(gtraits::is_shallow<TCh>::value);
    ASSERT_TRUE(gtraits::is_shallow<TUCh>::value);
}

// boolean
TEST(Traits, type_traitsTBool) {
    ASSERT_TRUE(gtraits::is_shallow<TBool>::value);
}

TEST(Traits, type_traitsTStr) {
    ASSERT_FALSE(gtraits::is_shallow<TStr>::value);
}

TEST(Traits, type_traitsTPair) {
    ASSERT_TRUE(gtraits::is_shallow<TIntPr>::value);
    ASSERT_FALSE(gtraits::is_shallow<TIntStrPr>::value);
}

#endif
