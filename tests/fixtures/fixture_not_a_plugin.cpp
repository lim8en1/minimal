// A valid shared object that is not a minimal plugin at all -- exports none of
// the required symbols. Must be reported, not crash the loader.
extern "C" int minimal_unrelated_symbol() { return 42; }
