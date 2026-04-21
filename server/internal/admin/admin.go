package admin

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"strconv"
	"strings"

	"realm-crafter/server/internal/db"
)

// Server exposes a JSON HTTP API on listenAddr for the GUE editor.
type Server struct {
	db         *db.DB
	listenAddr string
}

func New(database *db.DB, listenAddr string) *Server {
	return &Server{db: database, listenAddr: listenAddr}
}

func (s *Server) Start(ctx context.Context) error {
	mux := http.NewServeMux()

	mux.HandleFunc("/admin/items", s.handleItems)
	mux.HandleFunc("/admin/items/", s.handleItemByID)

	srv := &http.Server{Addr: s.listenAddr, Handler: corsMiddleware(mux)}
	go func() {
		<-ctx.Done()
		_ = srv.Shutdown(context.Background())
	}()
	log.Printf("admin: HTTP API listening on %s", s.listenAddr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return fmt.Errorf("admin: %w", err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

func (s *Server) handleItems(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		items, err := s.db.ListItemTemplates(r.Context())
		if err != nil {
			jsonError(w, err.Error(), http.StatusInternalServerError)
			return
		}
		jsonOK(w, items)

	case http.MethodPost:
		var t db.ItemTemplate
		if err := json.NewDecoder(r.Body).Decode(&t); err != nil {
			jsonError(w, "invalid JSON: "+err.Error(), http.StatusBadRequest)
			return
		}
		if strings.TrimSpace(t.Name) == "" {
			jsonError(w, "name is required", http.StatusBadRequest)
			return
		}
		id, err := s.db.CreateItemTemplate(r.Context(), &t)
		if err != nil {
			jsonError(w, err.Error(), http.StatusInternalServerError)
			return
		}
		t.ID = id
		w.WriteHeader(http.StatusCreated)
		jsonOK(w, t)

	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (s *Server) handleItemByID(w http.ResponseWriter, r *http.Request) {
	idStr := strings.TrimPrefix(r.URL.Path, "/admin/items/")
	id, err := strconv.Atoi(idStr)
	if err != nil || id <= 0 {
		jsonError(w, "invalid id", http.StatusBadRequest)
		return
	}

	switch r.Method {
	case http.MethodPut:
		var t db.ItemTemplate
		if err := json.NewDecoder(r.Body).Decode(&t); err != nil {
			jsonError(w, "invalid JSON: "+err.Error(), http.StatusBadRequest)
			return
		}
		t.ID = id
		if strings.TrimSpace(t.Name) == "" {
			jsonError(w, "name is required", http.StatusBadRequest)
			return
		}
		if err := s.db.UpdateItemTemplate(r.Context(), &t); err != nil {
			jsonError(w, err.Error(), http.StatusInternalServerError)
			return
		}
		jsonOK(w, t)

	case http.MethodDelete:
		if err := s.db.DeleteItemTemplate(r.Context(), id); err != nil {
			jsonError(w, err.Error(), http.StatusConflict)
			return
		}
		w.WriteHeader(http.StatusNoContent)

	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func corsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func jsonOK(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func jsonError(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}
