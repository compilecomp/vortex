#include "vortex_test.hpp"

#include "vortex/support/tagged_value.hpp"

using vortex::TaggedValue;

VORTEX_TEST(tagged_smi_roundtrip) {
    const TaggedValue v = TaggedValue::smi(42);
    VORTEX_EXPECT(v.is_smi());
    VORTEX_EXPECT_EQ(v.as_smi(), 42);
    VORTEX_EXPECT_EQ(TaggedValue::smi(-7).as_smi(), -7);
    VORTEX_EXPECT_EQ(TaggedValue::smi(0).as_smi(), 0);
}

VORTEX_TEST(tagged_smi_range) {
    VORTEX_EXPECT_EQ(TaggedValue::smi(TaggedValue::smi_min()).as_smi(),
                     TaggedValue::smi_min());
    VORTEX_EXPECT_EQ(TaggedValue::smi(TaggedValue::smi_max()).as_smi(),
                     TaggedValue::smi_max());
}

VORTEX_TEST(tagged_specials) {
    VORTEX_EXPECT(TaggedValue::null().is_null());
    VORTEX_EXPECT(TaggedValue::undefined().is_undefined());
    VORTEX_EXPECT(TaggedValue::boolean(true).is_boolean());
    VORTEX_EXPECT(TaggedValue::boolean(true).as_boolean_unchecked());
    VORTEX_EXPECT(!TaggedValue::boolean(false).as_boolean_unchecked());
    // Distinct bit patterns for every immediate.
    VORTEX_EXPECT(TaggedValue::null().raw() != TaggedValue::undefined().raw());
    VORTEX_EXPECT(TaggedValue::null().raw() != TaggedValue::boolean(true).raw());
    VORTEX_EXPECT(TaggedValue::boolean(true).raw() != TaggedValue::boolean(false).raw());
}

VORTEX_TEST(tagged_truthiness) {
    VORTEX_EXPECT(!TaggedValue::smi(0).truthy());
    VORTEX_EXPECT(TaggedValue::smi(1).truthy());
    VORTEX_EXPECT(TaggedValue::boolean(true).truthy());
    VORTEX_EXPECT(!TaggedValue::boolean(false).truthy());
    VORTEX_EXPECT(!TaggedValue::null().truthy());
    VORTEX_EXPECT(!TaggedValue::undefined().truthy());
}

VORTEX_TEST(tagged_reference_equality) {
    VORTEX_EXPECT(TaggedValue::null().reference_equals(TaggedValue::null()));
    VORTEX_EXPECT(!TaggedValue::smi(1).reference_equals(TaggedValue::smi(2)));
}
