# Fase 3 - Etapa C - Smoke Checklist (Party)

Objetivo: validar ciclo completo de party com 2 jogadores, sem relogar.

## 1) Setup

1. Subir servidor (`dist/server/server.exe`).
2. Abrir 2 clientes (`dist/client/rco_client.exe`).
3. Entrar no mesmo area com dois personagens: PlayerA e PlayerB.

## 2) Fluxo principal

1. PlayerA convida PlayerB (UI ou `/party invite PlayerB`).
2. PlayerB aceita (`Accept Invite` ou `/party accept`).
3. Validar painel de party nos dois clientes:
- mesmo `party_id`
- lider correto
- 2 membros
4. PlayerA transfere lideranca para PlayerB (`Make Lead` ou `/party lead PlayerB`).
5. PlayerB remove PlayerA (`Kick` ou `/party kick PlayerA`).
6. PlayerB sai da party (`Leave Party` ou `/party leave`).

## 3) Regras de validacao

1. Nao pode convidar a si mesmo.
2. Nao pode convidar player offline.
3. Nao pode convidar player em outra area.
4. Convite nao bloqueia por distancia no baseline atual (somente contexto de area).
5. Apenas lider pode convidar/kick/transferir lider.
6. Nao deve haver estado zumbi apos disconnect.

## 4) UX esperada

1. Notificacoes aparecem no topo do cliente.
2. Mensagens de erro aparecem em vermelho.
3. Mensagens informativas aparecem em verde claro.
4. Convite pendente aparece no painel do alvo.

## 5) Resultado esperado

1. Convite -> aceite -> transferencia -> kick -> leave conclui sem relogar.
2. Estado de membros/lider permanece coerente nos dois clientes.
3. Tentativas ilegais recebem feedback visual e nao alteram o estado da party.

## 6) Validacao de XP por proximidade

1. Formar party com 3 jogadores no mesmo area: Killer, Near e Far.
2. Posicionar Near dentro do raio de share e Far fora do raio.
3. Killer mata um NPC.
4. Validar que Killer e Near recebem `PXPUpdate`; Far nao recebe ganho.
5. Repetir kill com total de XP impar e confirmar que o resto fica com o Killer.
