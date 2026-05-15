package net

import "strings"

func normalizeActorName(name string) string {
	return strings.ToLower(strings.TrimSpace(name))
}

func (s *Server) registerInGameClient(c *ClientConn) {
	if s == nil || c == nil || c.actor == nil {
		return
	}
	charID := strings.TrimSpace(c.actor.CharacterID)
	nameKey := normalizeActorName(c.actor.Name)
	runtimeID := c.actor.RuntimeID
	if charID == "" || nameKey == "" {
		return
	}

	s.clientRegistryMu.Lock()
	s.clientsByCharacter[charID] = c
	s.clientsByName[nameKey] = c
	if runtimeID != 0 {
		s.clientsByRuntimeID[runtimeID] = c
	}
	s.clientRegistryMu.Unlock()
}

func (s *Server) unregisterInGameClient(c *ClientConn) {
	if s == nil || c == nil || c.actor == nil {
		return
	}
	charID := strings.TrimSpace(c.actor.CharacterID)
	nameKey := normalizeActorName(c.actor.Name)
	if charID == "" && nameKey == "" {
		return
	}

	s.clientRegistryMu.Lock()
	if charID != "" {
		delete(s.clientsByCharacter, charID)
	}
	if nameKey != "" {
		if current, ok := s.clientsByName[nameKey]; ok && current == c {
			delete(s.clientsByName, nameKey)
		}
	}
	if c.actor.RuntimeID != 0 {
		if current, ok := s.clientsByRuntimeID[c.actor.RuntimeID]; ok && current == c {
			delete(s.clientsByRuntimeID, c.actor.RuntimeID)
		}
	}
	s.clientRegistryMu.Unlock()
}

func (s *Server) findClientByCharacterID(characterID string) *ClientConn {
	charID := strings.TrimSpace(characterID)
	if s == nil || charID == "" {
		return nil
	}
	s.clientRegistryMu.RLock()
	c := s.clientsByCharacter[charID]
	s.clientRegistryMu.RUnlock()
	return c
}

func (s *Server) findClientByName(name string) *ClientConn {
	nameKey := normalizeActorName(name)
	if s == nil || nameKey == "" {
		return nil
	}
	s.clientRegistryMu.RLock()
	c := s.clientsByName[nameKey]
	s.clientRegistryMu.RUnlock()
	return c
}

func (s *Server) findClientByRuntimeID(runtimeID uint32) *ClientConn {
	if s == nil || runtimeID == 0 {
		return nil
	}
	s.clientRegistryMu.RLock()
	c := s.clientsByRuntimeID[runtimeID]
	s.clientRegistryMu.RUnlock()
	return c
}
