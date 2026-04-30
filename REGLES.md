# Protocole de travail OBLIGATOIRE

## Règle absolue
Tu utilises le système de critique externe pour CHAQUE problème non trivial.
Tu ne codes JAMAIS sans validation préalable de ton analyse.
Tu disposes de **35 outils MCP** pour t'aider — utilise-les.

## Identification du projet
- Utilise le nom du dossier courant comme `project_id`
- Si l'utilisateur donne un nom de projet, utilise celui-là

---

## 🧠 Démarrage de session : la critic DB est ta mémoire long-terme

**Avant de raisonner sur quoi que ce soit dans un projet déjà connu, interroge d'abord la critic database.**

Trois couches de mémoire avec des durées de vie très différentes :

| Couche | Durée de vie | Détail | À utiliser quand |
|---|---|---|---|
| 1. Conversation active | Volatile, 3/4 perdu à la compression | Riche | En cours de session, dialogue immédiat |
| 2. `MEMORY.md` auto | Persistant entre sessions | Une ligne par fait | Profil user, préférences, raccourcis projet |
| 3. **Critic database** | **Immortel** | **Investigations entières (50–100 KB), décisions archivées avec raisonnement** | **Démarrage de session, contexte dégradé, avant de relancer une voie** |

**Tu as tendance à ne raisonner qu'aux niveaux 1 et 2. Ne fais pas cette erreur.** Le niveau 3 est le plus riche et c'est lui qui te sauve quand tout le reste est dégradé.

### Règles à appliquer systématiquement

1. **Au démarrage d'une session sur un projet déjà connu, ou si tu sens que le contexte est dégradé** :
   ```
   critic_history({ project_id, limit: 20 })
   ```
   AVANT de raisonner, AVANT de proposer une approche, AVANT de relancer une investigation.

2. **Avant de re-tenter une voie technique** ou de relancer une investigation similaire :
   ```
   critic_search_findings({ query: "...", project_id, limit: 10 })
   ```
   Et pour les job_id repérés, charge le détail avec `critic_results({ job_id })`.

3. **Quand tu résumes l'historique au user, cite les `job_id` et `decision_id`**, pas seulement ta mémoire interne. Ça te rend traçable et permet à l'user de retrouver lui-même.

4. **Quand une décision importante est prise pendant la session**, archive-la AUSSITÔT via `critic_decision` — avec son raisonnement complet, pas juste l'output final. C'est ce raisonnement qui sauve les sessions futures.

5. **Quand un finding est confirmé par test** → `critic_validate_finding({ finding_id, validated: true, note: "..." })` (déclenche un push Dify automatique).

6. **Quand une investigation a abouti à une conclusion exploitable** (un fix appliqué, une architecture choisie) → `critic_add_report` pour la consolider en un document unique pour la KB Dify.

### Exemple type

User : "On reprend le projet NPU tap, où on en était ?"

❌ Mauvais réflexe : se baser sur `MEMORY.md` + ma mémoire interne et inventer un état approximatif.

✅ Bon réflexe :
```
1. critic_history({ project_id: "npu-tap", limit: 20 })
   → liste les analyses, reviews, décisions récentes
2. critic_search_findings({ query: "NPU tap V2 NO-GO", project_id: "npu-tap" })
   → retrouve l'investigation qui a conclu au NO-GO
3. critic_results({ job_id: "<celui repéré>" })
   → charge les findings détaillés des workers
4. Je résume au user : "D'après le job 64a4b28e du 2026-04-XX, V2 a été classé NO-GO car [raison]. La décision archivée 18e10a2c indique que [...]."
```

---

## Vue d'ensemble des outils disponibles

| Catégorie | Outils | Quand l'utiliser |
|-----------|--------|------------------|
| **Critique sync** | `critic_analyze`, `critic_review`, `critic_review_batch`, `critic_history`, `critic_decision` | Validation rapide d'analyse ou de code |
| **Recherche rapide** | `critic_research` | Question technique synchrone : Dify + Firecrawl + 1 LLM de synthèse |
| **Critique async** | `critic_submit`, `critic_status`, `critic_results` | Bug hunts, analyses lourdes, reviews multi-fichiers, investigations |
| **Base de connaissances** | `critic_search_findings`, `critic_validate_finding` | Recherche sémantique dans les findings passés (cumulatif) |
| **Rapports & Dify** | `critic_add_report`, `critic_dify_status` | Rédiger des rapports consolidés et pousser dans la KB Dify |
| **Recherche code** | `code_search`, `code_context` | Trouver du code par sens (sémantique) |
| **Recherche docs** | `doc_query`, `doc_list` | Interroger les datasheets et docs techniques (PDFs indexés) |
| **Git** | `git_summary`, `git_risk`, `git_changelog`, `project_digest` | Comprendre l'activité d'un repo |
| **TODOs** | `todo_list` | Voir les TODO/FIXME/HACK classifiés par sévérité |
| **Build** | `build_start`, `build_status`, `build_errors` | Lancer/surveiller des builds (Yocto, etc.) |
| **Performance** | `perf_record`, `perf_compare`, `perf_trend`, `regression_check` | Détecter les régressions de perf/taille |
| **Tests/spawn** | `test_run`, `test_status`, `test_results`, `claude_spawn`, `claude_spawn_status`, `claude_spawn_results` | Lancer des tests ou un Claude Code distant en fond |

---

## IMPORTANT : utiliser le serveur, pas le poste local

Quand l'utilisateur demande une analyse, un bug hunt, une review ou dit "clone", "lance", "en fond", "sur le serveur" :
- Appelle l'outil MCP IMMÉDIATEMENT
- NE LIS PAS les fichiers locaux pour les envoyer toi-même
- NE LANCE PAS d'agents Explore en local
- Le serveur a déjà tout indexé (code, docs, TODOs) ou peut cloner les repos lui-même
- Les outils async sont NON-BLOQUANTS : tu retournes le job_id et tu continues

---

## Phase 1 : Analyse (OBLIGATOIRE avant de coder)

1. Décris ton approche en détail :
   - Quel est le problème exact ?
   - Quelles sont tes hypothèses ?
   - Quelle solution proposes-tu et pourquoi ?
   - Quelles alternatives as-tu écartées et pourquoi ?
2. Appelle `critic_analyze` avec ton analyse
3. ATTENDS la réponse. NE CODE PAS.

## Phase 2 : Correction (si non approuvé)
1. Lis CHAQUE contradiction et erreur identifiée
2. Ne défends PAS ton approche initiale par défaut
3. Révise ton analyse en tenant compte des critiques
4. Re-soumets via `critic_analyze`
5. Maximum 3 itérations — si toujours rejeté, demande à l'utilisateur de trancher

## Phase 3 : Implémentation (seulement si approuvé)
1. Code UNIQUEMENT l'approche validée
2. Ne dévie PAS de l'analyse approuvée sans re-soumettre

## Phase 4 : Review de code
1. Appelle `critic_review` avec le code produit (un seul fichier) ou `critic_review_batch` (plusieurs)
2. Corrige les issues identifiées
3. En cas de changement majeur, retourne en Phase 1

## Phase 5 : Décision
1. Quand un choix important est fait, appelle `critic_decision`
2. Documente le raisonnement derrière chaque décision

---

## Phase 6 : Bug hunt et analyses lourdes (async, NON-BLOQUANT)

Quand l'utilisateur demande une analyse approfondie, un bug hunt, ou une review multi-fichiers :

1. Utilise `critic_submit` (PAS `critic_analyze`) — c'est NON-BLOQUANT
2. Spécifie `git_url` si le code est sur un repo git distant — les workers clonent le repo sur le serveur
3. Spécifie `file_paths` pour les fichiers à analyser dans le repo
4. Spécifie `search_patterns` pour que les workers trouvent aussi les fichiers connexes
5. Spécifie `dify_datasets` et `dify_queries` pour injecter du contexte des datasheets/specs (cf. section dédiée)
6. Les workers tournent en arrière-plan, même toute la nuit si nécessaire
7. Utilise `critic_status` pour vérifier l'avancement
8. Utilise `critic_results` pour récupérer les résultats terminés

### Les types de jobs et leurs workers

| `job_type` | Workers lancés | Quand utiliser |
|-----------|----------------|-----------------|
| **`investigation`** | **6 cerveaux IA actifs** : 5 OpenCode (glm-5.1, kimi-k2.5, qwen3.5:397b, minimax-m2.7, deepseek-v4-pro) + Claude Code CLI | Le PLUS PUISSANT. Les workers fouillent activement le repo (read, grep, follow). Idéal pour les bugs complexes où il faut suivre des appels de fonction dans plusieurs fichiers. |
| **`bug_hunt`** | 4 LLMs passifs : kimi-k2-thinking, glm-5.1, minimax-m2.7, qwen3-coder:480b | Reçoivent tout le code en one-shot. Bon quand le bug est cerné et tu veux 4 avis profonds sur un code spécifique. |
| **`review`** | qwen3-coder:480b + glm-5.1 + minimax-m2.7 + deepseek-coder local + deepseek-v4-flash | Review de code après implémentation |
| **`analyze`** | qwen3.5:397b + kimi-k2.5 | Critique d'une approche/analyse avant de coder |
| **`custom`** | glm-5.1 + qwen3:32b local | Tâche libre |

### Quand sync vs async ?
- **investigation** → TOUJOURS async (`critic_submit` avec `job_type: "investigation"`) — c'est le plus puissant
- **bug_hunt** → TOUJOURS `critic_submit` (4 cerveaux IA sont plus puissants que toi seul)
- **review multi-fichiers** → `critic_submit` avec `job_type: "review"`
- **analyse d'approche simple** → `critic_analyze` (sync, rapide)
- **review d'un seul petit fichier** → `critic_review` (sync)
- L'utilisateur dit "lance", "soumets", "en fond", "cette nuit", "fouille", "investigue" → `critic_submit`

### Investigation vs bug_hunt — différence clé
- **investigation** : 6 agents qui **lisent eux-mêmes** le repo sur le serveur. Ils peuvent suivre des références, vérifier leurs hypothèses, lire d'autres fichiers à la volée. Plus lent (~5-15 min) mais **beaucoup plus puissant**.
- **bug_hunt** : 4 LLMs passifs qui reçoivent tout le code dans le prompt initial. Plus rapide (~1-3 min) mais limité au code que tu leur envoies.

**Règle** : si l'utilisateur dit juste "bug hunt" sans préciser, et que c'est un bug complexe nécessitant de l'investigation → utilise `investigation` (c'est plus puissant). Réserve `bug_hunt` aux cas où tu sais exactement quels fichiers contiennent le bug.

### 📚 Bases de connaissances Dify

Tu peux injecter du contexte des datasheets et specs dans les prompts des workers. C'est essentiel pour les bugs hardware où la datasheet a la vérité.

**Bases disponibles** (utilise les noms friendly) :
| Nom | Contenu |
|-----|---------|
| `imx8mp-trm` | Datasheet/TRM i.MX8MP officielle (14M mots) |
| `imx8mp-sai7-sof` | IMX8MP SAI7 AUDIOMIX DSP SOF Firmware (9M mots) |
| `sof-doc` | Documentation officielle thesofproject |
| `aidocgen` | Contenu vérifié IMX8MP/SAI7/SOF |
| `audio-effects` | Effets audio par IA |
| `audio-code` | Code de traitement de son par IA |

**Comment ça marche** : pour chaque query dans `dify_queries`, le système récupère les top-5 chunks pertinents de chaque dataset dans `dify_datasets` et les injecte dans le prompt initial de chaque worker.

### 🌐 Scraping web avec Firecrawl

Tu peux scraper des pages web pour fournir du contexte externe aux workers. Idéal pour :
- **Code Linux upstream** (pour comparer avec SOF) : `https://github.com/torvalds/linux/blob/master/drivers/...`
- **Errata NXP** et notes d'application
- **Issues/PRs GitHub** : `https://github.com/thesofproject/sof/issues?q=...`
- **Forums NXP/SOF/communautés**
- **Datasheets en ligne** (PDFs convertis en markdown)
- **Documentation upstream** (Linux device tree, ASoC, etc.)

**Paramètre** : `firecrawl_urls` (liste d'URLs). Les pages sont scrapées en markdown et injectées dans le prompt initial de chaque worker.

**Quand utiliser** :
- Le bug touche un domaine où Linux a une implémentation upstream à comparer → scraper le code Linux
- L'utilisateur soupçonne un bug connu → chercher dans les issues GitHub
- Tu veux vérifier une convention/spec → scraper la doc officielle
- Une datasheet contient l'info clé → scraper la page de la datasheet en ligne

### Exemple investigation MAXIMALE (code + datasheets + web scraping)
```
critic_submit({
  project_id: "mon-projet",
  job_type: "investigation",
  priority: "high",
  
  // Code source à investiguer
  git_url: "https://github.com/thesofproject/sof.git",
  file_paths: ["src/drivers/imx/sdma.c", "src/drivers/imx/sai.c"],
  search_patterns: ["drivers/imx"],
  
  // Datasheets/specs depuis Dify
  dify_datasets: ["imx8mp-trm", "imx8mp-sai7-sof"],
  dify_queries: [
    "SDMA watermark register g_reg[7]",
    "SAI7 RX FIFO depth and watermark",
    "TDM 8 slots configuration"
  ],
  
  // Pages web à scraper avec Firecrawl
  firecrawl_urls: [
    "https://github.com/torvalds/linux/blob/master/drivers/dma/imx-sdma.c",
    "https://github.com/thesofproject/sof/issues?q=SDMA+watermark+TDM",
    "https://community.nxp.com/t5/i-MX-Processors/SAI-TDM-watermark/td-p/1234567"
  ],
  
  description: "Description du bug...",
  context: "Contexte technique..."
})
```

Avec cet exemple, chaque worker (les 6 cerveaux IA) reçoit dans son prompt initial :
1. La description du bug
2. Le contexte technique
3. Les chunks pertinents des datasheets Dify
4. Le contenu scrapé des pages web Firecrawl
5. Les fichiers de départ du repo

Et **en plus**, ils peuvent fouiller activement le repo cloné. C'est la combinaison la plus puissante.

### Exemple bug_hunt simple
```
critic_submit({
  project_id: "mon-projet",
  job_type: "bug_hunt",
  priority: "high",
  git_url: "https://github.com/org/repo.git",
  file_paths: ["src/driver.c", "src/dma.c", "include/driver.h"],
  description: "Description du bug...",
  context: "Contexte technique..."
})
```

### Suivre l'avancement et relancer
- L'utilisateur a plusieurs boutons dans https://mcp.electrosens.fr (admin web) sur chaque job :
  - **🔄 Relancer** : cancel l'ancien (si running) + crée un nouveau job avec le même payload
  - **🛑 Kill** : tue les processes opencode/gemini/claude/clangd et annule le job
  - **Cancel** : marque cancelled en DB (soft, laisse les processes continuer)
  - **Delete** : supprime le job et ses résultats
- Si un worker reste bloqué trop longtemps (> 15 min), il finit par timeout et le job continue avec les autres

### 🧠 Base de connaissances cumulative (`critic_search_findings`)

**TOUS** les résultats d'investigation et bug_hunt sont **automatiquement embedded** (bge-m3 + pgvector) et stockés dans la table `investigation_findings`. Avant de relancer une investigation, **cherche d'abord** si on a déjà trouvé quelque chose de similaire.

De plus, à chaque nouvelle investigation, le système **injecte automatiquement** dans le prompt initial des workers les findings passés les plus similaires (top-5 avec similarité > 55%). C'est du savoir cumulatif gratuit. Tu peux désactiver avec `skip_past_findings: true`.

**Outils disponibles :**
```
critic_search_findings({
  query: "SDMA watermark TDM bug",    // recherche sémantique
  project_id: "yocto-nxp-debix",      // filtre optionnel
  repo_name: "sof",                   // filtre optionnel
  severity: "critical",               // filtre optionnel
  limit: 10,
  with_full_text: false               // true pour avoir le texte complet
})

critic_validate_finding({
  finding_id: "uuid-du-finding",
  validated: true,                    // true=validé, false=réfuté
  note: "fix appliqué et testé OK"
})
```

**Quand utiliser `critic_search_findings`** :
- Avant de lancer une nouvelle investigation sur un sujet similaire → vérifie si on a déjà la réponse
- Pour retrouver une analyse qu'on a faite il y a plusieurs jours/semaines
- Pour voir ce que les IA ont dit sur un domaine spécifique (ex: "bugs liés au cache cohérence")

**Règle** : quand l'utilisateur te parle d'un bug qu'il a déjà mentionné, ou d'un sujet technique récurrent, fais `critic_search_findings` AVANT de relancer une investigation.

### 📤 Sync automatique vers Dify (`critic_validate_finding`, `critic_add_report`, `critic_dify_status`)

Quand un finding est **validé** (`critic_validate_finding` avec `validated: true`), il est **automatiquement poussé** dans une base de connaissances Dify dédiée au project. Idem pour les rapports consolidés rédigés via `critic_add_report`. L'utilisateur peut ensuite créer manuellement un chatbot Dify qui interroge cette KB.

**Granularité** : **un dataset Dify par `project_id`**, nommé `critic-<project_id>`. Créé automatiquement à la première utilisation (`ensureProjectDataset`).

**Déduplication** : hash SHA-256 du titre+contenu. Un même contenu ne part qu'une fois.

**Outils :**
```
// Valider + auto-push un finding existant
critic_validate_finding({
  finding_id: "uuid",
  validated: true,
  note: "confirmé par test hardware"
})
// → retourne { ok: true, finding, dify_push: { status: 'success', dify_document_id } }

// Rédiger un rapport consolidé (tu synthétises plusieurs findings en un document exploitable)
critic_add_report({
  project_id: "yocto-nxp-debix",
  title: "Bug SAI7 TDM 8 slots — cause racine et fix",
  content: "# Analyse\n\nLe bug provient de...\n\n## Fix\n\nPatch dans sai.c...",
  tags: ["SAI", "TDM", "i.MX8MP"],
  related_findings: ["uuid1", "uuid2"],  // optionnel
  related_jobs: ["uuid-job"],             // optionnel
  push_to_dify: true                      // par défaut true
})

// Voir l'état du sync pour un projet
critic_dify_status({ project_id: "yocto-nxp-debix" })
// → retourne { dataset: {...}, stats: {total, success, skipped, failed}, recent_pushes, dataset_url }
```

**Workflow** :
1. Investigation → 5-6 findings bruts par worker stockés dans `investigation_findings`
2. Tu lis les findings, identifies le consensus, et rédiges **un rapport** via `critic_add_report`
3. Le rapport part automatiquement dans Dify
4. Si un finding individuel est vraiment actionnable et validé par test, utilise `critic_validate_finding` — il part aussi dans Dify
5. L'utilisateur crée manuellement un Chat/Agent dans l'UI Dify qui branche le dataset `critic-<project_id>`

**Règle de rédaction de rapport** : privilégie `critic_add_report` (rapport synthétique) à la validation en masse de findings bruts. Les findings bruts sont redondants (même bug vu par 6 workers) ; un rapport est une source de vérité consolidée.

### ⚠️ Comment lire correctement une review/investigation

**NE SAUTE PAS aux "issues" sans lire le verdict final.** Les reviews ont souvent :
- Une section `issues` / `findings` listant des points potentiels
- Un `overall_assessment` / `overall` / `verdict` qui est la conclusion réelle

Exemple piège :
```json
{
  "issues": [4 issues...],           // 4 issues listées
  "overall_assessment": "L'implémentation est FONCTIONNELLEMENT CORRECTE. Les 3 issues sont 'low' priority, juste de la robustesse défensive."
}
```

**→ Le code est OK**, pas "4 bugs à corriger". Les sévérités `low`/`medium` sont souvent des suggestions défensives, pas des bugs bloquants. Lis TOUJOURS le `overall_assessment` / `overall` avant de conclure.

Si tu as un doute sur une review, **relis-la en entier** ou dis à l'utilisateur "les workers ont signalé X issues de sévérité low/medium mais le verdict global est Y" — laisse-le trancher.

---

## Recherche rapide (`critic_research`) — synchrone, 30s à 3min

**Quand utiliser** : tu as une question technique précise et tu veux une réponse RAPIDE sourcée sur :
- Datasheets/specs Dify pré-indexées
- Pages web ciblées (URL connue)
- Recherche web Google-like via Firecrawl

C'est **PAS** une investigation, **PAS** un clone git, **PAS** 6 agents qui fouillent un repo. Juste : question → 3 sources → synthèse par UN LLM.

**Différence avec `critic_submit job_type=investigation`** :
- `critic_research` : 30s-3min, synchrone, juste un Q/R sourcé
- `investigation` : 5-30min, async, 7 agents qui lisent activement un repo cloné

**Paramètres principaux** :
```
critic_research({
  question: "Le SDMA3 de l'i.MX8MP a-t-il une limite de 1024 mots par transfert AP2AP ?",

  // Au moins UNE de ces sources (sinon réponse vide)
  dify_datasets: ["imx8mp-trm", "sof-doc"],         // bases Dify (noms friendly)
  dify_queries: ["SDMA AP2AP transfer size limit"], // optionnel, sinon utilise question
  firecrawl_urls: ["https://github.com/torvalds/linux/blob/master/drivers/dma/imx-sdma.c"],
  firecrawl_search: "i.MX8MP SDMA AP2AP max transfer errata",  // recherche web
  firecrawl_search_limit: 3,                        // top-N (défaut 3, max 10)

  model: "glm-5.1",                                 // optionnel, défaut glm-5.1
  system_prompt: "..."                              // optionnel, prompt système custom
})
```

**Quand l'utilisateur dit** : "vérifie", "cherche", "est-ce que la datasheet dit X", "renseigne-toi" → `critic_research`.
**Quand il dit** : "fouille", "investigue", "trouve le bug" → `critic_submit job_type=investigation`.

---

## Recherche sémantique dans le code (`code_search`, `code_context`)

Le serveur indexe en continu tous les repos dans `/root/projects/` avec des embeddings (bge-m3 + pgvector). **Avant de faire un `Grep` ou un `find`, essaye `code_search`** — c'est plus malin :

- `Grep` cherche des mots exacts → tu rates les concepts formulés autrement
- `code_search` cherche par **sens** → trouve même si les mots ne correspondent pas

### Quand utiliser
- "Où est gérée la synchronisation DMA ?" → `code_search`
- "Trouve les fichiers qui font de la calibration audio" → `code_search`
- "Montre-moi le contexte autour de cette fonction" → `code_context`

### Exemples
```
code_search({ query: "buffer overflow protection in DMA transfer", repo_name: "sof", limit: 10 })
code_context({ repo_name: "sof", file_path: "src/drivers/imx/sai.c", start_line: 100, end_line: 150 })
```

---

## Interroger la documentation technique (`doc_query`, `doc_list`)

Les datasheets, specs et docs PDF dans `/root/docs/` sont indexées (qwen3-vl pour les images, bge-m3 pour le texte).

### Quand utiliser
- "Quel est le registre de config du SAI sur i.MX8MP ?" → `doc_query`
- "Que dit la datasheet sur le mode TDM ?" → `doc_query`
- L'utilisateur cite une spec, une datasheet, un standard → `doc_query` AVANT de répondre de mémoire

### Exemples
```
doc_query({ query: "SAI watermark configuration TDM mode", limit: 5 })
doc_list({})  // pour voir ce qui est indexé
```

---

## Intelligence Git (`git_summary`, `git_risk`, `git_changelog`, `project_digest`)

### Quand utiliser
- "Qu'est-ce qui a bougé sur cette branche cette semaine ?" → `git_summary`
- "Est-ce que ce merge est risqué ?" → `git_risk`
- "Génère un changelog" → `git_changelog`
- "Que s'est-il passé hier sur le projet ?" → `project_digest` (résumé quotidien généré chaque nuit)

### Exemples
```
git_summary({ repo_path: "/root/projects/sof", days: 7 })
git_risk({ repo_path: "/root/projects/sof", branch: "feature-tdm" })
git_changelog({ repo_path: "/root/projects/sof", from_ref: "v2.5", to_ref: "HEAD" })
project_digest({ project_name: "sof", days: 3 })
```

---

## Suivi des TODO/FIXME (`todo_list`)

Le serveur scanne tous les repos toutes les 5 minutes et classifie les TODO/FIXME/HACK/XXX/BUG/WARN par sévérité avec mistral-small3.2.

### Quand utiliser
- "Quels sont les TODO critiques ?" → `todo_list({ severity: "critical" })`
- "Liste les FIXME du driver audio" → `todo_list({ tag: "FIXME", repo_name: "sof" })`
- L'utilisateur dit "il faut nettoyer" ou "il y a des trucs à corriger" → `todo_list` d'abord

```
todo_list({ repo_name: "sof", severity: "high" })
todo_list({ tag: "FIXME", show_resolved: false })
```

---

## Build et erreurs de build (`build_start`, `build_status`, `build_errors`)

Lance des builds longs (Yocto, bitbake, make) avec analyse IA des erreurs.

### Quand utiliser
- "Lance le build Yocto" → `build_start`
- "Où en est le build ?" → `build_status`
- "Pourquoi ça a fail ?" → `build_errors` (deepseek-coder analyse l'erreur)

### Exemples
```
build_start({ project_name: "yocto-debix", command: "bitbake imx-image-core", working_dir: "/root/projects/yocto-nxp-debix" })
build_status({ project_name: "yocto-debix" })
build_errors({ project_name: "yocto-debix", last_n: 5 })
```

---

## Métriques et régressions (`perf_record`, `perf_compare`, `perf_trend`, `regression_check`)

### Quand utiliser
- Après un build réussi → `perf_record` pour enregistrer la taille du firmware, le compile time, etc.
- "Le firmware a-t-il grossi ?" → `perf_compare` ou `regression_check`
- "Tendance de la taille du binaire ?" → `perf_trend`

### Exemples
```
perf_record({
  project_name: "sof",
  build_ref: "v2.5-rc1",
  metrics: [
    { name: "firmware_size", value: 524288, unit: "bytes" },
    { name: "compile_time", value: 145, unit: "seconds" }
  ]
})
perf_compare({ project_name: "sof", current_ref: "v2.5-rc1", baseline_ref: "v2.4" })
regression_check({ project_name: "sof", binary_path: "/root/projects/sof/build/sof.bin" })
```

---

## Tests et Claude Code distant (`test_run`, `claude_spawn`)

### `test_run` — lance des tests en arrière-plan
- Le test tourne sur le serveur, n'est pas bloquant
- Utile pour des suites longues
```
test_run({ project_name: "sof", command: "make test", working_dir: "/root/projects/sof" })
test_status({ project_name: "sof" })
test_results({ run_id: "..." })
```

### `claude_spawn` — lance une instance de Claude Code sur le serveur
**ULTRA puissant** : tu peux déléguer une tâche complexe à un autre Claude qui tourne sur le serveur, avec accès direct au repo cloné. Pratique pour :
- Investigations profondes qui prendraient trop de tours
- Refactos complexes
- Génération de tests en masse
- Tâches qui peuvent tourner toute la nuit

```
claude_spawn({
  project_id: "sof",
  task: "Analyse en profondeur le driver SAI dans src/drivers/imx/sai.c. Identifie tous les bugs potentiels, propose un patch correctif, et crée un test unitaire qui reproduit le bug TDM 8 slots.",
  working_dir: "/root/projects/sof"
})
claude_spawn_status({ project_id: "sof" })
claude_spawn_results({ job_id: "..." })
```

⚠️ Le `claude_spawn` lance un Claude indépendant qui peut faire à peu près ce qu'il veut sur le serveur. Utilise-le pour des tâches bien définies.

---

## Règles comportementales

### Avant de répondre
1. Si la question concerne du **code existant** → utilise `code_search` AVANT de faire un Grep
2. Si la question concerne une **doc/datasheet** → utilise `doc_query` AVANT de répondre de mémoire
3. Si tu vas **modifier du code** → suis le protocole Phase 1-5
4. Si c'est un **bug complexe** → utilise `critic_submit` en bug_hunt

### Pendant le travail
- Si le critique dit que tu as déjà essayé cette approche : ÉCOUTE-LE
- Si le critique trouve une contradiction avec tes dires précédents : CORRIGE-TOI
- Si le critique cite la documentation : la doc a RAISON, pas toi
- Consulte `critic_history` si tu as un doute sur ce qui a déjà été tenté
- En cas de conflit entre ta conviction et le critique, présente les deux à l'utilisateur

### Privilégier le serveur
- Tu travailles depuis le poste local mais le serveur a **tout** indexé
- N'envoie pas de gros payloads de code via SSH si le code est déjà sur le serveur
- Les outils async sont là pour ça : tu envoies une description, le serveur fait le boulot
