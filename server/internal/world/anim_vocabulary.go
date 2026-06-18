package world

import "sync"

var (
	animVocabMu     sync.RWMutex
	animVocabParent map[string]string
)

// SetAnimVocabulary installs the animation-vocabulary fallback map: action
// name -> parent action name. An action with no entry (or an empty parent) is
// a root with no fallback.
//
// Phase A.1: loaded at boot and exposed via AnimFallbackParent, but not yet
// consulted by BroadcastAnimate (a later commit wires the runtime fallback).
func SetAnimVocabulary(parentByName map[string]string) {
	animVocabMu.Lock()
	defer animVocabMu.Unlock()
	animVocabParent = parentByName
}

// AnimFallbackParent returns the parent action name for name in the
// animation vocabulary tree, or "" if name is a root or not in the
// vocabulary.
func AnimFallbackParent(name string) string {
	animVocabMu.RLock()
	defer animVocabMu.RUnlock()
	return animVocabParent[name]
}
