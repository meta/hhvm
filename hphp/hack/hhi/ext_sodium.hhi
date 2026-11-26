<?hh

final class SodiumException extends Exception {}

///// utilities

<<__PHPStdLib>>
function sodium_bin2hex(string $binary): string;

<<__PHPStdLib>>
function sodium_hex2bin(string $hex, ?string $ignore = null): string;

<<__PHPStdLib>>
function sodium_memzero(inout mixed $buffer): void;

<<__PHPStdLib>>
function sodium_increment(inout mixed $buffer): void;

<<__PHPStdLib>>
function sodium_add(inout mixed $value, string $add): void;

<<__PHPStdLib>>
function sodium_memcmp(string $a, string $b): int;

<<__PHPStdLib>>
function sodium_compare(string $a, string $b): int;

<<__PHPStdLib>>
function sodium_crypto_scalarmult(string $n, string $p): string;

///// Hashing (not for passwords or keys)

<<__PHPStdLib>>
function sodium_crypto_generichash_keygen(): string;

<<__PHPStdLib>>
function sodium_crypto_generichash(
  string $msg,
  ?string $key = null,
  ?int $length = null,
): string;

<<__PHPStdLib>>
function sodium_crypto_generichash_init(
  ?string $key = null,
  ?int $length = null,
): string;

<<__PHPStdLib>>
function sodium_crypto_generichash_update(
  inout mixed $state,
  string $msg,
): bool;

<<__PHPStdLib>>
function sodium_crypto_generichash_final(
  inout mixed $state,
  ?int $length = null,
): string;

<<__PHPStdLib>>
function sodium_crypto_shorthash_keygen(): string;

<<__PHPStdLib>>
function sodium_crypto_shorthash(string $message, string $key): string;

///// Key derivation (kdf)

<<__PHPStdLib>>
function sodium_crypto_pwhash(
  int $output_length,
  string $password,
  string $salt,
  int $opslimit,
  int $memlimit,
): string;

<<__PHPStdLib>>
function sodium_crypto_pwhash_scryptsalsa208sha256(
  int $output_length,
  string $password,
  string $salt,
  int $opslimit,
  int $memlimit,
): string;

<<__PHPStdLib>>
function sodium_crypto_kdf_derive_from_key(
  int $subkey_length,
  int $subkey_id,
  string $context,
  string $key,
): string;

<<__PHPStdLib>>
function sodium_crypto_core_hchacha20(
  string $in,
  string $k,
  ?string $c = null,
): string;

///// Password hashing for storage

<<__PHPStdLib>>
function sodium_crypto_pwhash_str(
  string $password,
  int $opslimit,
  int $memlimit,
): string;

<<__PHPStdLib>>
function sodium_crypto_pwhash_str_verify(string $hash, string $password): bool;

<<__PHPStdLib>>
function sodium_crypto_pwhash_scryptsalsa208sha256_str(
  string $password,
  int $opslimit,
  int $memlimit,
): string;

<<__PHPStdLib>>
function sodium_crypto_pwhash_scryptsalsa208sha256_str_verify(
  string $hash,
  string $password,
): bool;

///// MACs

<<__PHPStdLib>>
function sodium_crypto_auth_keygen(): string;

<<__PHPStdLib>>
function sodium_crypto_auth(string $message, string $key): string;

<<__PHPStdLib>>
function sodium_crypto_auth_verify(
  string $signature,
  string $message,
  string $key,
): bool;

///// Symmetric (PSK) encryption

<<__PHPStdLib>>
function sodium_crypto_secretbox_keygen(): string;

<<__PHPStdLib>>
function sodium_crypto_secretbox(
  string $plaintext,
  string $nonce,
  string $key,
): string;

<<__PHPStdLib>>
function sodium_crypto_secretbox_open(
  string $ciphertext,
  string $nonce,
  string $key,
): mixed /* string | false */;

///// Asymetric (public-key) encryption key management

<<__PHPStdLib>>
function sodium_crypto_box_keypair(): string;

<<__PHPStdLib>>
function sodium_crypto_box_seed_keypair(string $seed): string;

<<__PHPStdLib>>
function sodium_crypto_box_keypair_from_secretkey_and_publickey(
  string $secretkey,
  string $publickey,
): string;

<<__PHPStdLib>>
function sodium_crypto_box_publickey(string $keypair): string;

<<__PHPStdLib>>
function sodium_crypto_box_secretkey(string $keypair): string;

<<__PHPStdLib>>
function sodium_crypto_box_publickey_from_secretkey(string $key): string;

<<__PHPStdLib>>
function sodium_crypto_scalarmult_base(string $key): string;

<<__PHPStdLib>>
function sodium_crypto_kx_keypair(): string;
<<__PHPStdLib>>
function sodium_crypto_kx_seed_keypair(string $seed): string;
<<__PHPStdLib>>
function sodium_crypto_kx_secretkey(string $keypair): string;
<<__PHPStdLib>>
function sodium_crypto_kx_publickey(string $keypair): string;
<<__PHPStdLib>>
function sodium_crypto_kx_client_session_keys(
  string $client_keypair,
  string $server_pubkey,
): (string, string);
<<__PHPStdLib>>
function sodium_crypto_kx_server_session_keys(
  string $server_keypair,
  string $client_pubkey,
): (string, string);

///// Unauthenticated asymetric (PSK) encryption (you probably don't want this)

<<__PHPStdLib>>
function sodium_crypto_stream(int $length, string $nonce, string $key): string;

<<__PHPStdLib>>
function sodium_crypto_stream_xor(
  string $message,
  string $nonce,
  string $key,
): string;

///// Asymetric (public-key) encryption

<<__PHPStdLib>>
function sodium_crypto_box(
  string $plaintext,
  string $nonce,
  string $keypair,
): string;

<<__PHPStdLib>>
function sodium_crypto_box_open(
  string $ciphertext,
  string $nonce,
  string $key,
): mixed;

<<__PHPStdLib>>
function sodium_crypto_box_seal(string $plaintext, string $publickey): string;

<<__PHPStdLib>>
function sodium_crypto_box_seal_open(
  string $ciphertext,
  string $keypair,
): mixed;

///// Asymetric (public key) signature key management

<<__PHPStdLib>>
function sodium_crypto_sign_keypair(): string;

<<__PHPStdLib>>
function sodium_crypto_sign_seed_keypair(string $seed): string;

<<__PHPStdLib>>
function sodium_crypto_sign_publickey_from_secretkey(string $secretkey): string;

<<__PHPStdLib>>
function sodium_crypto_sign_keypair_from_secretkey_and_publickey(
  string $secretkey,
  string $publickey,
): string;

<<__PHPStdLib>>
function sodium_crypto_sign_publickey(string $keypair): string;

<<__PHPStdLib>>
function sodium_crypto_sign_secretkey(string $keypair): string;

<<__PHPStdLib>>
function sodium_crypto_sign_ed25519_pk_to_curve25519(string $eddsakey): string;
<<__PHPStdLib>>
function sodium_crypto_sign_ed25519_sk_to_curve25519(string $eddsakey): string;

///// Ed25519 primitives.

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_is_valid_point(string $point): bool;

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_add(string $point_a, string $point_b): string;

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_sub(string $point_a, string $point_b): string;

<<__PHPStdLib>>
function sodium_crypto_scalarmult_ed25519_noclamp(string $scalar, string $point): string;

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_scalar_reduce(string $scalar): string;

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_scalar_add(string $scalar_a, string $scalar_b): string;

<<__PHPStdLib>>
function sodium_crypto_core_ed25519_scalar_mul(string $scalar_a, string $scalar_b): string;

<<__PHPStdLib>>
function sodium_crypto_scalarmult_ed25519_base(string $scalar): string;

<<__PHPStdLib>>
function sodium_crypto_scalarmult_ed25519_base_noclamp(string $scalar): string;

///// Asymetric (public key) signatures

<<__PHPStdLib>>
function sodium_crypto_sign(string $message, string $secretkey): string;

<<__PHPStdLib>>
function sodium_crypto_sign_open(string $signed, string $publickey): mixed;

<<__PHPStdLib>>
function sodium_crypto_sign_detached(
  string $message,
  string $secretkey,
): string;

<<__PHPStdLib>>
function sodium_crypto_sign_verify_detached(
  string $signature,
  string $message,
  string $publickey,
): bool;

///// AEAD functions

<<__PHPStdLib>>
function sodium_crypto_aead_aes256gcm_is_available(): bool;

<<__PHPStdLib>>
function sodium_crypto_aead_aes256gcm_decrypt(
  string $ciphertext,
  string $ad,
  string $pnonce,
  string $key,
): mixed;

<<__PHPStdLib>>
function sodium_crypto_aead_aes256gcm_encrypt(
  string $plaintext,
  string $ad,
  string $pnonce,
  string $key,
): string;

<<__PHPStdLib>>
function sodium_crypto_aead_chacha20poly1305_decrypt(
  string $ciphertext,
  string $ad,
  string $pnonce,
  string $key,
): mixed;

<<__PHPStdLib>>
function sodium_crypto_aead_chacha20poly1305_encrypt(
  string $plaintext,
  string $ad,
  string $pnonce,
  string $key,
): string;

<<__PHPStdLib>>
function sodium_crypto_aead_chacha20poly1305_ietf_decrypt(
  string $ciphertext,
  string $ad,
  string $pnonce,
  string $key,
): mixed;

<<__PHPStdLib>>
function sodium_crypto_aead_chacha20poly1305_ietf_encrypt(
  string $plaintext,
  string $ad,
  string $pnonce,
  string $key,
): string;

<<__PHPStdLib>>
function sodium_crypto_aead_xchacha20poly1305_ietf_decrypt(
  string $ciphertext,
  string $ad,
  string $pnonce,
  string $key,
): mixed;

<<__PHPStdLib>>
function sodium_crypto_aead_xchacha20poly1305_ietf_encrypt(
  string $plaintext,
  string $ad,
  string $pnonce,
  string $key,
): string;

///// AEAD functions for stream

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_keygen(): string;

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_init_push(
  string $key,
): (string, string); /* state, header */

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_push(
  inout mixed $state,
  string $plaintext,
  string $ad,
  int $tag,
): string;

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_init_pull(
  string $key,
  string $header,
): string;

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_pull(
  inout mixed $state,
  string $ciphertext,
  string $ad,
): (string, int); /* plaintext, tag */

<<__PHPStdLib>>
function sodium_crypto_secretstream_xchacha20poly1305_rekey(
  inout mixed $state,
): void;

///// Ristretto

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_add(string $p, string $q): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_sub(string $p, string $q): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_is_valid_point(string $s): bool;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_random(): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_from_hash(string $r): string;

<<__PHPStdLib>>
function sodium_crypto_scalarmult_ristretto255(string $n, string $p): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_reduce(string $s): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_random(): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_invert(string $s): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_negate(string $s): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_complement(string $s): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_add(
  string $x,
  string $y,
): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_sub(
  string $x,
  string $y,
): string;

<<__PHPStdLib>>
function sodium_crypto_core_ristretto255_scalar_mul(
  string $x,
  string $y,
): string;

///// Always-defined constants

const string SODIUM_LIBRARY_VERSION;
const int SODIUM_LIBRARY_MAJOR_VERSION;
const int SODIUM_LIBRARY_MINOR_VERSION;

const int SODIUM_CRYPTO_SCALARMULT_BYTES;
const int SODIUM_CRYPTO_SCALARMULT_SCALARBYTES;

const int SODIUM_CRYPTO_GENERICHASH_KEYBYTES;
const int SODIUM_CRYPTO_GENERICHASH_KEYBYTES_MIN;
const int SODIUM_CRYPTO_GENERICHASH_KEYBYTES_MAX;
const int SODIUM_CRYPTO_GENERICHASH_BYTES;
const int SODIUM_CRYPTO_GENERICHASH_BYTES_MIN;
const int SODIUM_CRYPTO_GENERICHASH_BYTES_MAX;

const int SODIUM_CRYPTO_SHORTHASH_BYTES;
const int SODIUM_CRYPTO_SHORTHASH_KEYBYTES;

const string SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_STRPREFIX;
const int SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_SALTBYTES;
const int SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_OPSLIMIT_INTERACTIVE;
const int SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_MEMLIMIT_INTERACTIVE;
const int SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_OPSLIMIT_SENSITIVE;
const int SODIUM_CRYPTO_PWHASH_SCRYPTSALSA208SHA256_MEMLIMIT_SENSITIVE;

const int SODIUM_CRYPTO_AUTH_BYTES;
const int SODIUM_CRYPTO_AUTH_KEYBYTES;

const int SODIUM_CRYPTO_SECRETBOX_KEYBYTES;
const int SODIUM_CRYPTO_SECRETBOX_MACBYTES;
const int SODIUM_CRYPTO_SECRETBOX_NONCEBYTES;

const int SODIUM_CRYPTO_BOX_SECRETKEYBYTES;
const int SODIUM_CRYPTO_BOX_PUBLICKEYBYTES;
const int SODIUM_CRYPTO_BOX_KEYPAIRBYTES;
const int SODIUM_CRYPTO_BOX_MACBYTES;
const int SODIUM_CRYPTO_BOX_NONCEBYTES;
const int SODIUM_CRYPTO_BOX_SEEDBYTES;

const int SODIUM_CRYPTO_SIGN_BYTES;
const int SODIUM_CRYPTO_SIGN_SEEDBYTES;
const int SODIUM_CRYPTO_SIGN_PUBLICKEYBYTES;
const int SODIUM_CRYPTO_SIGN_SECRETKEYBYTES;
const int SODIUM_CRYPTO_SIGN_KEYPAIRBYTES;

const int SODIUM_CRYPTO_STREAM_NONCEBYTES;
const int SODIUM_CRYPTO_STREAM_KEYBYTES;

const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_KEYBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_NSECBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_NPUBBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_ABYTES;

const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_ABYTES;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_HEADERBYTES;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_KEYBYTES;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_MESSAGEBYTES_MAX;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_TAG_MESSAGE;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_TAG_PUSH;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_TAG_REKEY;
const int SODIUM_CRYPTO_SECRETSTREAM_XCHACHA20POLY1305_TAG_FINAL;

///// Conditionally-defined constants (depends on libsodium build options)

const int SODIUM_CRYPTO_PWHASH_SALTBYTES;
const string SODIUM_CRYPTO_PWHASH_STRPREFIX;
const int SODIUM_CRYPTO_PWHASH_OPSLIMIT_INTERACTIVE;
const int SODIUM_CRYPTO_PWHASH_MEMLIMIT_INTERACTIVE;
const int SODIUM_CRYPTO_PWHASH_OPSLIMIT_MODERATE;
const int SODIUM_CRYPTO_PWHASH_MEMLIMIT_MODERATE;
const int SODIUM_CRYPTO_PWHASH_OPSLIMIT_SENSITIVE;
const int SODIUM_CRYPTO_PWHASH_MEMLIMIT_SENSITIVE;

const int SODIUM_CRYPTO_BOX_SEALBYTES;

const int SODIUM_CRYPTO_AEAD_AES245GCM_KEYBYTES;
const int SODIUM_CRYPTO_AEAD_AES245GCM_NSECBYTES;
const int SODIUM_CRYPTO_AEAD_AES245GCM_NPUBBYTES;
const int SODIUM_CRYPTO_AEAD_AES245GCM_ABYTES;

const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_IETF_KEYBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_IETF_NSECBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_IETF_NPUBBYTES;
const int SODIUM_CRYPTO_AEAD_CHACHA20POLY1305_IETF_ABYTES;

const int SODIUM_CRYPTO_AEAD_XCHACHA20POLY1305_IETF_KEYBYTES;
const int SODIUM_CRYPTO_AEAD_XCHACHA20POLY1305_IETF_NSECBYTES;
const int SODIUM_CRYPTO_AEAD_XCHACHA20POLY1305_IETF_NPUBBYTES;
const int SODIUM_CRYPTO_AEAD_XCHACHA20POLY1305_IETF_ABYTES;

const int SODIUM_CRYPTO_KDF_BYTES_MIN;
const int SODIUM_CRYPTO_KDF_BYTES_MAX;
const int SODIUM_CRYPTO_KDF_BYTES_CONTEXTBYTES;
const int SODIUM_CRYPTO_KDF_BYTES_KEYBYTES;

const int SODIUM_CRYPTO_CORE_HCHACHA20_INPUTBYTES;
const int SODIUM_CRYPTO_CORE_HCHACHA20_KEYBYTES;
const int SODIUM_CRYPTO_CORE_HCHACHA20_OUTPUTBYTES;
const int SODIUM_CRYPTO_CORE_HCHACHA20_CONSTBYTES;

const int SODIUM_CRYPTO_KX_PUBLICKEYBYTES;
const int SODIUM_CRYPTO_KX_SESSIONKEYBYTES;
const int SODIUM_CRYPTO_KX_SECRETKEYBYTES;
const int SODIUM_CRYPTO_KX_KEYPAIRBYTES;

const int SODIUM_CRYPTO_SCALARMULT_RISTRETTO255_BYTES;
const int SODIUM_CRYPTO_SCALARMULT_RISTRETTO255_SCALARBYTES;
const int SODIUM_CRYPTO_CORE_RISTRETTO255_BYTES;
const int SODIUM_CRYPTO_CORE_RISTRETTO255_HASHBYTES;
const int SODIUM_CRYPTO_CORE_RISTRETTO255_SCALARBYTES;
const int SODIUM_CRYPTO_CORE_RISTRETTO255_NONREDUCEDSCALARBYTES;
