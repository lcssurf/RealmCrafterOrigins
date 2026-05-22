# Débito Técnico — RCO MMO Engine

Lista de pendências reconhecidas durante desenvolvimento. Cada item tem origem 
(quando virou débito) e quando vale atacar. Manter atualizado.

## 1. Split de `server/internal/db/db.go`

**Estado:** arquivo monolítico com ~4500 linhas em 2026-05-16.  
**Contém:** structs de domínio + 25+ migrations + funções CRUD de items, characters, 
spells, quests, party, abilities, weapon kits, etc — tudo misturado.

**Sintomas:**
- Cada mudança em DB toca o mesmo arquivo
- Cognitivo: difícil localizar funções
- Performance da IA: contexto longo reduz qualidade de edits

**Plano proposto:**
Split por domínio em `server/internal/db/`:
- `db.go` (Open, struct DB, helpers)
- `migrations.go` (todas as migrateV*)
- `items.go`, `characters.go`, `spells.go`, `parties.go`, `quests.go`, `weapons.go`, etc

**Quando atacar:** antes da próxima feature que não seja Weapon Kits.  
**Estimativa:** 6-8 commits, 2-3 dias.

## 2. Migração `spell_templates` → `ability_templates`

**Estado:** dois sistemas de skill coexistem.
- Legado: `spell_templates` + `character_known_spells` + `PCastSpell` + spellbar UI
- Novo: `ability_templates` + cast_intent runtime + Weapon Kits

**Bridge atual:** `spell_templates.runtime_ability_id` opcional faz spell legada 
disparar via runtime moderno. Funcional mas frágil.

**Plano proposto:**
- Após Weapon Kits funcional no client (Commits 10-17 da feature atual), avaliar
- Migrar player skills para `ability_templates` exclusivamente
- Aposentar `spell_templates`, `character_known_spells`, `PCastSpell`, aba Spells no GUE
- Hotbar legado removido

**Quando atacar:** após Commit 17 da feature Weapon Kits, ou quando spells virarem 
empecilho perceptível.  
**Estimativa:** 2 semanas, varia conforme spells/Lua em uso.

## 3. DBs duplicados no projeto

**Estado:** existem 3 cópias de `rco.db`:
- `dist/server/rco.db` — DB real, usado por server e GUE
- `server/rco.db` — vazio (0 bytes), legado de teste antigo
- `server/.gocache/.../rco.db` — DB temporário criado pelos `go test`

**Sintomas:**
- Confusão em validação ("IA validou em qual DB?")
- Risco de migrations rodarem em DB errado
- Já causou ~1 dia perdido no Commit 7 da feature Weapon Kits

**Plano proposto:**
- Adicionar regras explícitas em `.gitignore` para todos os `rco.db`
- Deletar `server/rco.db` vazio
- Documentar: "o DB autoritativo é `dist/server/rco.db`. Nenhum outro deve ser usado."

**Quando atacar:** trivial, qualquer hora. 15 minutos.

## 4. Processo: validação em DB real, não em cópia

**Estado:** durante Commits 1.5 / 7 da Weapon Kits, a IA executora reportou validação 
em cópia local porque o DB real estava readonly. Migrations não rodaram no DB real. 
Bug "Create não funciona" custou ~2h de diagnóstico.

**Regra para o futuro:**
- Validação de migration: sempre rodar no `dist/server/rco.db` que GUE/jogo realmente abrem
- Se ambiente bloquear (readonly, lock), parar e reportar — não substituir por cópia silenciosamente
- Aplicar a todo commit que toque schema

**Quando atacar:** já em vigor para próximos commits.

## 7. Lição: nomenclatura captura intent

Durante Commit 14 da feature Weapon Kits, criei coluna `max_skills_in_loadout`
em equipment_slot_config. Nome ambíguo: sugere "tamanho do pool de skills disponíveis"
ao mesmo tempo que "quantos slots no hotbar". Renomeei para `hotbar_slots_granted`
no Commit 15.5 quando a confusão ficou explícita.

Lição para próximos schemas: ao nomear campo, pergunte "este nome distingue de
todos os outros conceitos similares no sistema?". Se há ambiguidade conceitual,
o nome carrega o débito.

---

Reportar:
- Path completo do arquivo criado
- Confirmação que conteúdo bate exatamente com o pedido
- Sem modificar outros arquivos

## 11. Categorias de skill nao-damage sem runtime

Schema de `ability_templates` suporta categories: `damage`, `heal`, `buff`, `debuff`, `mobility`, `utility`, `summon`.
Mas o runtime de combate so processa `damage` hoje.

Quando a primeira skill de outra categoria for criada, o runtime precisa ser estendido:
- `heal`: aplicar cura no target (nao dano negativo)
- `buff`: aplicar status effect com duracao no caster/ally
- `debuff`: status effect no target
- `mobility`: movimento fisico (dash/charge) com regras de cancelamento
- `utility`: efeitos diversos (taunt, interrupt, etc)
- `summon`: spawn de pet/companion temporario

Mastery tambem precisa adaptar semantica de `mastery_primary_bonus_per_lvl` por categoria
(dano, cura, duracao/potencia, etc).

## 12. Ability templates editadas via GUE nao recarregam runtime

Hoje, mudancas em `ability_templates` via GUE exigem restart do server para recarregar
o catalogo runtime.

Quando edicao de skills em tempo de execucao virar frequente, precisamos de um mecanismo
seguro de reload (manual trigger, versionamento ou hot-reload controlado) para evitar
divergencia entre DB e runtime ativo.

## 13. Schema de ability_templates duplicado entre server e GUE

Server cria tabela via `migrateV19` + `migrateV30`. GUE tambem cria via
`EnsureTables` em `combat_abilities.cpp`. Quando schema mudar (novos campos),
precisa atualizar 2 lugares - risco de divergencia.

Decidir futuro:
- GUE deixa de criar tabela (assume que server ja criou) - mais simples
- GUE e server compartilham migrations via arquivo SQL unico - mais robusto
- Manter status quo, aceitar duplicacao consciente

## 14. Retry de SQLite BUSY aplicado apenas em UpsertCharacterSkillProgress

Hotfix 30.5 adicionou retry-with-backoff em `UpsertCharacterSkillProgress`
porque essa funcao e chamada em hot path (hook de combat) e perdia XP.

Outras funcoes de escrita podem ter o mesmo problema mas nao foram protegidas:
- AbilityCooldowns (in-memory, sem risco de SQLite)
- ApplyDamage (HP em memoria; se persistido futuramente, candidato)
- Updates em LastCombatAt (em memoria)

Padrao sugerido para futuro: criar helper `d.executeWithRetry(ctx, query, args...)`
que aplique retry automatico para `ExecContext`. Migrar funcoes criticas conforme
necessario.

Trigger: quando aparecer outro caso de "dado perdido por BUSY", migrar mais
funcoes para o helper.

## 15. Anim bindings faltando em combat

Configurar anim bindings `Hit`, `AttackBasic` e `AttackHeavyWindup` nos
Appearance dos NPCs e player. Hoje isso ainda gera warnings
`missing_action_binding` durante o combate.

## 16. Anim bindings Dodge/Guard/Parry ausentes (fallback Idle)

Adicionar bindings de animacao `Dodge`, `Guard` e `Parry` nos appearances de
personagens e NPCs. Hoje o runtime usa fallback `Idle`, o que evita travamento
de estado, mas nao reproduz anim visual especifica para a acao defensiva.

## 17. `character_primary_stats_per_level` hoje e referencia de NPC/fallback

Com primary stats persistidos por personagem, a tabela
`character_primary_stats_per_level` deixa de ser fonte principal para player e
fica como referencia de defaults por level (NPC/fallback).

Se isso virar confuso no fluxo de dados, considerar separar explicitamente em
uma tabela dedicada para NPCs ou renomear para refletir o novo papel.

## 18. UI de distribuicao de stats/respec pendente no cliente

Backend de distribuicao de pontos e respec foi preparado, mas a interface de
cliente (sheet/botoes/feedback) fica para o commit 38b.

Enquanto isso, os pacotes existem e o estado e persistido, mas o jogador final
nao tem fluxo visual para consumir a feature.

## 19. Cache de progression/primary stats exige restart

`CharacterProgressionRuntimeConfig` e cache de primary stats por level sao
carregados no startup do server.

Edicoes feitas via GUE/SQL em runtime nao refletem imediatamente sem restart.
Se isso virar gargalo, planejar mecanismo de reload controlado.
