#include <cassert>
#include <iostream>

#include "kernel/catalog.hpp"

using namespace knk;

namespace {

void add_entity_then_find_entity_returns_the_id() {
    Catalog catalog;

    catalog.add_entity(1, Value::of_text("Alice"));

    auto found = catalog.find_entity(Value::of_text("Alice"));

    assert(found.has_value());
    assert(*found == 1);
}

void find_entity_for_unknown_value_returns_nullopt() {
    Catalog catalog;

    catalog.add_entity(1, Value::of_text("Alice"));

    assert(!catalog.find_entity(Value::of_text("Bob")).has_value());
    assert(!catalog.find_entity(Value::of_int64(42)).has_value());
}

void entity_value_returns_the_interned_value_for_a_known_id() {
    Catalog catalog;

    catalog.add_entity(1, Value::of_int64(42));

    auto value = catalog.entity_value(1);

    assert(value.has_value());
    assert(*value == Value::of_int64(42));
    assert(!catalog.entity_value(2).has_value());
}

void distinct_value_kinds_with_similar_content_are_distinct_entities() {
    Catalog catalog;

    catalog.add_entity(1, Value::of_text("42"));
    catalog.add_entity(2, Value::of_int64(42));

    assert(catalog.find_entity(Value::of_text("42")) == std::optional<EntityId>(1));
    assert(catalog.find_entity(Value::of_int64(42)) == std::optional<EntityId>(2));
}

void next_entity_id_advances_past_the_highest_added_id() {
    Catalog catalog;

    assert(catalog.next_entity_id() == 1);

    catalog.add_entity(1, Value::of_text("Alice"));
    assert(catalog.next_entity_id() == 2);

    catalog.add_entity(5, Value::of_text("Bob"));
    assert(catalog.next_entity_id() == 6);

    catalog.add_entity(3, Value::of_text("Acme"));
    assert(catalog.next_entity_id() == 6);
}

void add_predicate_then_find_predicate_returns_the_id() {
    Catalog catalog;

    catalog.add_predicate(1, "works_at");

    auto found = catalog.find_predicate("works_at");

    assert(found.has_value());
    assert(*found == 1);
}

void find_predicate_for_unknown_name_returns_nullopt() {
    Catalog catalog;

    catalog.add_predicate(1, "works_at");

    assert(!catalog.find_predicate("lives_in").has_value());
}

void predicate_name_returns_the_interned_name_for_a_known_id() {
    Catalog catalog;

    catalog.add_predicate(1, "works_at");

    auto name = catalog.predicate_name(1);

    assert(name.has_value());
    assert(*name == "works_at");
    assert(!catalog.predicate_name(2).has_value());
}

void next_predicate_id_advances_past_the_highest_added_id() {
    Catalog catalog;

    assert(catalog.next_predicate_id() == 1);

    catalog.add_predicate(1, "works_at");
    assert(catalog.next_predicate_id() == 2);

    catalog.add_predicate(5, "lives_in");
    assert(catalog.next_predicate_id() == 6);
}

} // namespace

int main() {
    add_entity_then_find_entity_returns_the_id();
    find_entity_for_unknown_value_returns_nullopt();
    entity_value_returns_the_interned_value_for_a_known_id();
    distinct_value_kinds_with_similar_content_are_distinct_entities();
    next_entity_id_advances_past_the_highest_added_id();
    add_predicate_then_find_predicate_returns_the_id();
    find_predicate_for_unknown_name_returns_nullopt();
    predicate_name_returns_the_interned_name_for_a_known_id();
    next_predicate_id_advances_past_the_highest_added_id();

    std::cout << "All catalog tests passed.\n";
}
