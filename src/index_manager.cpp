#include <kernel/index_manager.hpp>

namespace knk {

void IndexManager::add(const Assertion& assertion) {
	subject_index_[assertion.subject].push_back(assertion.id);
	SubjectPredicateKey key {
		assertion.subject,
		assertion.predicate
	};
	current_index_[key].push_back(assertion.id);
}


}