/*
 * decay.c — effective relevance and retrieval scoring (spec §3.4, §5.3).
 *
 * Identity never decays: asper_half_life_s returns 0 for it, which every
 * helper here treats as "no decay" (factor 1.0). The spec defines scoring
 * only for context/project (identity bypasses retrieval), but ANY-scans
 * (recall, search) still score identity records: with half-life 0 the
 * formula degenerates to w_sim*cos + w_rel*relevance + w_rec*1.0.
 */

#include <math.h>
#include <string.h>

#include "asper_internal.h"

int64_t asper_half_life_s(const asper_config *cfg, asper_section s)
{
    switch (s) {
    case ASPER_SECTION_CONTEXT:
        return cfg->context_half_life_s;
    case ASPER_SECTION_PROJECT:
        return cfg->project_half_life_s;
    default:
        return 0; /* identity: no decay (§3.2) */
    }
}

/* 0.5 ^ (dt / half_life); half_life <= 0 means no decay, negative dt
 * (clock skew, hand-edited timestamps) clamps to no decay. */
static double asper_decay_factor(int64_t dt_s, int64_t half_life_s)
{
    if (half_life_s <= 0 || dt_s <= 0)
        return 1.0;
    return pow(0.5, (double)dt_s / (double)half_life_s);
}

double asper_eff_relevance(const asper_config *cfg, const asper_record *r,
                           asper_time now)
{
    int64_t hl = asper_half_life_s(cfg, r->section);
    return r->relevance * asper_decay_factor(now - r->last_access, hl);
}

double asper_score_record(const asper_config *cfg, const asper_record *r,
                          double cos, asper_time now)
{
    int64_t hl = asper_half_life_s(cfg, r->section);
    double eff = r->relevance * asper_decay_factor(now - r->last_access, hl);
    double rec = asper_decay_factor(now - r->updated_at, hl);
    return cfg->w_similarity * cos + cfg->w_relevance * eff +
           cfg->w_recency * rec;
}

int asper_hit_cmp(const asper_hit *a, const asper_hit *b)
{
    if (a->score > b->score)
        return -1;
    if (a->score < b->score)
        return 1;
    if (a->rec->updated_at > b->rec->updated_at)
        return -1;
    if (a->rec->updated_at < b->rec->updated_at)
        return 1;
    return strcmp(a->rec->id, b->rec->id);
}
