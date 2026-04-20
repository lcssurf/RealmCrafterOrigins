package net

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"log"
	"math/big"
	"net"
	"time"

	"realm-crafter/server/internal/accounts"
	"realm-crafter/server/internal/db"
	"realm-crafter/server/internal/scripting"
	"realm-crafter/server/internal/world"

	"github.com/quic-go/quic-go"
)

// Config holds the QUIC listener configuration.
type Config struct {
	ListenAddr string
	CertFile   string
	KeyFile    string
}

// Server manages the QUIC listener and spawns a ClientConn per connection.
type Server struct {
	config    *Config
	db        *db.DB
	accounts  *accounts.Service
	world     *world.World
	scripting *scripting.Registry
}

// NewServer creates a Server with all required dependencies.
func NewServer(cfg *Config, database *db.DB, accts *accounts.Service, w *world.World, reg *scripting.Registry) *Server {
	return &Server{
		config:    cfg,
		db:        database,
		accounts:  accts,
		world:     w,
		scripting: reg,
	}
}

// Start binds the QUIC listener and begins accepting connections.
// It blocks until ctx is cancelled.
func (s *Server) Start(ctx context.Context) error {
	tlsCfg, err := s.buildTLSConfig()
	if err != nil {
		return fmt.Errorf("server: build TLS: %w", err)
	}

	quicCfg := &quic.Config{
		KeepAlivePeriod: 30 * time.Second,
		MaxIdleTimeout:  60 * time.Second,
	}

	listener, err := quic.ListenAddr(s.config.ListenAddr, tlsCfg, quicCfg)
	if err != nil {
		return fmt.Errorf("server: listen %s: %w", s.config.ListenAddr, err)
	}
	defer listener.Close()

	log.Printf("server: listening on %s", s.config.ListenAddr)

	for {
		conn, err := listener.Accept(ctx)
		if err != nil {
			if ctx.Err() != nil {
				// Context cancelled — clean shutdown.
				return nil
			}
			log.Printf("server: accept error: %v", err)
			continue
		}

		log.Printf("server: connection from %s", conn.RemoteAddr())
		client := &ClientConn{
			conn:   conn,
			server: s,
		}
		go client.Run(ctx)
	}
}

// ---------------------------------------------------------------------------
// TLS helpers
// ---------------------------------------------------------------------------

func (s *Server) buildTLSConfig() (*tls.Config, error) {
	if s.config.CertFile != "" && s.config.KeyFile != "" {
		cert, err := tls.LoadX509KeyPair(s.config.CertFile, s.config.KeyFile)
		if err != nil {
			return nil, fmt.Errorf("load key pair: %w", err)
		}
		return &tls.Config{
			Certificates: []tls.Certificate{cert},
			NextProtos:   []string{"rco"},
		}, nil
	}

	// Generate a self-signed certificate valid for 10 years.
	cert, err := generateSelfSignedCert()
	if err != nil {
		return nil, fmt.Errorf("self-signed cert: %w", err)
	}
	log.Println("server: using auto-generated self-signed TLS certificate")
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		NextProtos:   []string{"rco"},
	}, nil
}

// generateSelfSignedCert creates an ECDSA P-256 self-signed certificate valid
// for 10 years with SANs for localhost and 127.0.0.1.
func generateSelfSignedCert() (tls.Certificate, error) {
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("generate key: %w", err)
	}

	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("serial: %w", err)
	}

	now := time.Now()
	template := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			Organization:  []string{"RealmCrafter Origins"},
			CommonName:    "localhost",
		},
		NotBefore:             now,
		NotAfter:              now.Add(10 * 365 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              []string{"localhost"},
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1")},
	}

	certDER, err := x509.CreateCertificate(rand.Reader, &template, &template, &priv.PublicKey, priv)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("create cert: %w", err)
	}

	privDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return tls.Certificate{}, fmt.Errorf("marshal key: %w", err)
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: certDER})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: privDER})

	return tls.X509KeyPair(certPEM, keyPEM)
}
