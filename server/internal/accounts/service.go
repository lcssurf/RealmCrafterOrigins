package accounts

import (
	"context"
	"fmt"
	"regexp"
	"sync"

	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/protocol"

	"golang.org/x/crypto/bcrypt"
)

var usernameRe = regexp.MustCompile(`^[a-zA-Z0-9_]{3,32}$`)

// Service handles account registration, login, and online-state tracking.
type Service struct {
	db     *db.DB
	online sync.Map // key: username (string), value: struct{}
}

// New creates a new Service.
func New(database *db.DB) *Service {
	return &Service{db: database}
}

// Register creates a new account.
// Returns a result code (protocol.ResultOK on success) and any internal error.
func (s *Service) Register(ctx context.Context, username, password, email string) (uint8, error) {
	if !usernameRe.MatchString(username) {
		return protocol.ResultInvalidName, nil
	}

	// Check if username is already taken.
	existing, err := s.db.GetAccountByUsername(ctx, username)
	if err == nil && existing != nil {
		return protocol.ResultAccountExists, nil
	}

	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.DefaultCost)
	if err != nil {
		return protocol.ResultInvalidCreds, fmt.Errorf("accounts: bcrypt: %w", err)
	}

	_, err = s.db.CreateAccount(ctx, username, string(hash), email)
	if err != nil {
		// Treat unique violation as account-exists to avoid leaking DB errors.
		return protocol.ResultAccountExists, fmt.Errorf("accounts: create: %w", err)
	}

	return protocol.ResultOK, nil
}

// Login authenticates a user.
// Returns the account on success, or a result code indicating the failure reason.
func (s *Service) Login(ctx context.Context, username, password string) (*db.Account, uint8, error) {
	account, err := s.db.GetAccountByUsername(ctx, username)
	if err != nil {
		// User not found — return generic invalid-creds to prevent enumeration.
		return nil, protocol.ResultInvalidCreds, nil
	}

	if err := bcrypt.CompareHashAndPassword([]byte(account.PasswordHash), []byte(password)); err != nil {
		return nil, protocol.ResultInvalidCreds, nil
	}

	if account.IsBanned {
		return nil, protocol.ResultBanned, nil
	}

	if s.IsOnline(username) {
		return nil, protocol.ResultAlreadyOnline, nil
	}

	return account, protocol.ResultOK, nil
}

// SetOnline marks an account as currently connected.
func (s *Service) SetOnline(username string) {
	s.online.Store(username, struct{}{})
}

// SetOffline removes the online marker for an account.
func (s *Service) SetOffline(username string) {
	s.online.Delete(username)
}

// IsOnline returns true if the account is currently connected.
func (s *Service) IsOnline(username string) bool {
	_, ok := s.online.Load(username)
	return ok
}
