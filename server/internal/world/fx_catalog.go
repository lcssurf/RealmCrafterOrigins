package world

import (
	"sort"
	"strings"
	"sync"
)

// FXTemplate is one runtime FX definition loaded from db.fx_templates.
type FXTemplate struct {
	ID          int
	Key         string
	DisplayName string

	BurstCount      int
	StreamInterval  float32
	LifetimeSeconds float32

	SpeedMin       float32
	SpeedMax       float32
	VelocityBiasX  float32
	VelocityBiasY  float32
	VelocityBiasZ  float32
	VelocitySpread float32

	ColorStartR float32
	ColorStartG float32
	ColorStartB float32
	ColorStartA float32
	ColorEndR   float32
	ColorEndG   float32
	ColorEndB   float32
	ColorEndA   float32
	SizeStart   float32
	SizeEnd     float32
	TexturePath string

	Enabled bool
}

var (
	fxCatalogMu sync.RWMutex
	fxCatalog   = make(map[string]*FXTemplate)
)

// SetFXTemplateCatalog replaces the in-memory FX catalog (enabled rows only).
func SetFXTemplateCatalog(templates []FXTemplate) {
	next := make(map[string]*FXTemplate, len(templates))
	for i := range templates {
		t := templates[i]
		if !t.Enabled {
			continue
		}
		key := strings.TrimSpace(t.Key)
		if key == "" {
			continue
		}
		tpl := new(FXTemplate)
		*tpl = t
		next[key] = tpl
	}

	fxCatalogMu.Lock()
	fxCatalog = next
	fxCatalogMu.Unlock()
}

// GetFXTemplate resolves one template by key.
func GetFXTemplate(key string) (*FXTemplate, bool) {
	fxCatalogMu.RLock()
	defer fxCatalogMu.RUnlock()

	t, ok := fxCatalog[key]
	if !ok || t == nil {
		return nil, false
	}
	cp := *t
	return &cp, true
}

// ListFXTemplateKeys returns all currently cached keys (sorted).
func ListFXTemplateKeys() []string {
	fxCatalogMu.RLock()
	defer fxCatalogMu.RUnlock()

	keys := make([]string, 0, len(fxCatalog))
	for k := range fxCatalog {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
