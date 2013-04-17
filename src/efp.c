/*-
 * Copyright (c) 2012-2013 Ilya Kaliman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "elec.h"
#include "private.h"
#include "stream.h"

static enum efp_result
ff_to_efp(enum ff_res res)
{
	switch (res) {
		case FF_OK:
			return EFP_RESULT_SUCCESS;
		case FF_FILE_NOT_FOUND:
			return EFP_RESULT_FILE_NOT_FOUND;
		case FF_BAD_FORMAT:
			return EFP_RESULT_SYNTAX_ERROR;
		case FF_STRING_TOO_LONG:
			return EFP_RESULT_SYNTAX_ERROR;
		case FF_NO_PARAMETERS:
			return EFP_RESULT_UNKNOWN_FF_TYPE;
	};
	assert(0);
}

static int
find_link_index(const struct frag *frag, const char *name, size_t *idx)
{
	assert(frag);
	assert(name);
	assert(idx);

	for (size_t i = 0, k = 0; i < frag->n_atoms; i++)
		if (frag->atoms[i].label[0] == '*') {
			if (strcmp(frag->atoms[i].label, name) == 0) {
				*idx = k;
				return 1;
			}
			k++;
		}

	return 0;
}

static void
update_ff_atoms(struct ff *ff, const struct frag *frag)
{
	vec_t pos;

	for (size_t i = 0, k = 0; i < frag->n_atoms; i++) {
		const struct efp_atom *atom = frag->atoms + i;

		if (atom->label[0] == '*') {
			pos.x = atom->x;
			pos.y = atom->y;
			pos.z = atom->z;
			efp_ff_set_atom_pos(ff, frag->ff_offset + k, pos);
			k++;
		}
	}
}

static void
add_ff_gradient(struct frag *frag, const vec_t *ff_grad)
{
	for (size_t i = 0, k = 0; i < frag->n_atoms; i++) {
		const struct efp_atom *atom = frag->atoms + i;

		if (atom->label[0] == '*') {
			efp_add_force(frag, CVEC(atom->x), ff_grad + k, NULL);
			k++;
		}
	}
}

static enum efp_result
init_ff(struct efp *efp)
{
	enum ff_res ff_res;

	for (size_t i = 0, n_ff_cur = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;
		frag->ff_offset = n_ff_cur;
		n_ff_cur = 0;

		for (size_t j = 0; j < frag->n_atoms; j++) {
			const struct efp_atom *at = frag->atoms + j;

			if (at->label[0] == '*') {
				char name[32];
				memset(name, 0, sizeof(name));

				char *dst = name;
				const char *src = at->label + 3;

				while (*src && *src != '_')
					*dst++ = *src++;

				if ((ff_res = efp_ff_add_atom(efp->ff, name)))
					return ff_to_efp(ff_res);

				n_ff_cur++;
			}
		}

		for (size_t j = 0, k = 0; j < frag->n_atoms; j++) {
			const struct efp_atom *at = frag->atoms + j;

			if (at->label[0] == '*') {
				const char *ptr = strchr(at->label, '_');

				if (ptr) {
					size_t idx = (size_t)strtol(ptr + 1, NULL, 10) - 1;

					if (idx >= n_ff_cur)
						return EFP_RESULT_INDEX_OUT_OF_RANGE;

					if ((ff_res = efp_ff_add_bond(efp->ff,
							frag->ff_offset + k,
							frag->ff_offset + idx)))
						return ff_to_efp(ff_res);
				}

				k++;
			}
		}

		n_ff_cur += frag->ff_offset;
	}

	return EFP_RESULT_SUCCESS;
}

static enum efp_result
parse_topology_record(struct efp *efp, const char *str)
{
	enum ff_res ff_res;
	size_t idx1, link1, idx2, link2;
	char name1[32], name2[32];

	if (sscanf(str, "%zu %zu %32s %32s", &idx1, &idx2, name1, name2) < 4)
		return EFP_RESULT_SYNTAX_ERROR;

	if (idx1 == idx2)
		return EFP_RESULT_SYNTAX_ERROR;

	if (--idx1 >= efp->n_frag || --idx2 >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	const struct frag *frag1 = efp->frags + idx1;
	const struct frag *frag2 = efp->frags + idx2;

	if (!find_link_index(frag1, name1, &link1) ||
	    !find_link_index(frag2, name2, &link2))
		return EFP_RESULT_UNKNOWN_ATOM;

	if ((ff_res = efp_ff_add_bond(efp->ff, frag1->ff_offset + link1,
				frag2->ff_offset + link2)))
		return ff_to_efp(ff_res);

	efp_bvec_set(efp->links_bvec, idx1 * efp->n_frag + idx2);
	efp_bvec_set(efp->links_bvec, idx2 * efp->n_frag + idx1);

	return EFP_RESULT_SUCCESS;
}

static enum efp_result
parse_topology(struct efp *efp, const char *path)
{
	struct stream *stream;
	enum efp_result res = EFP_RESULT_SUCCESS;

	if ((stream = efp_stream_open(path)) == NULL)
		return EFP_RESULT_FILE_NOT_FOUND;

	while (!efp_stream_eof(stream)) {
		efp_stream_next_line(stream);
		efp_stream_skip_space(stream);

		if (efp_stream_eol(stream))
			continue;

		if ((res = parse_topology_record(efp, efp_stream_get_ptr(stream))))
			break;
	}

	efp_stream_close(stream);
	return res;
}

static int
check_rotation_matrix(const mat_t *rotmat)
{
	vec_t ax = { rotmat->xx, rotmat->yx, rotmat->zx };
	vec_t ay = { rotmat->xy, rotmat->yy, rotmat->zy };
	vec_t az = { rotmat->xz, rotmat->yz, rotmat->zz };

	if (!eq(vec_len(&ax), 1.0) ||
	    !eq(vec_len(&ay), 1.0) ||
	    !eq(vec_len(&az), 1.0))
		return 0;

	if (!eq(vec_dot(&ax, &ay), 0.0))
		return 0;

	vec_t cross = vec_cross(&ax, &ay);

	if (!eq(cross.x, az.x) ||
	    !eq(cross.y, az.y) ||
	    !eq(cross.z, az.z))
		return 0;

	return 1;
}

static void
points_to_matrix(const double *pts, mat_t *rotmat)
{
	vec_t p1 = { pts[0], pts[1], pts[2] };
	vec_t p2 = { pts[3], pts[4], pts[5] };
	vec_t p3 = { pts[6], pts[7], pts[8] };

	vec_t r12 = vec_sub(&p2, &p1);
	vec_t r13 = vec_sub(&p3, &p1);

	vec_normalize(&r12);
	vec_normalize(&r13);

	double dot = vec_dot(&r12, &r13);

	r13.x -= dot * r12.x;
	r13.y -= dot * r12.y;
	r13.z -= dot * r12.z;

	vec_t cross = vec_cross(&r12, &r13);

	vec_normalize(&r13);
	vec_normalize(&cross);

	rotmat->xx = r12.x, rotmat->xy = r13.x, rotmat->xz = cross.x;
	rotmat->yx = r12.y, rotmat->yy = r13.y, rotmat->yz = cross.y;
	rotmat->zx = r12.z, rotmat->zy = r13.z, rotmat->zz = cross.z;
}

static void
update_fragment(struct frag *frag)
{
	/* update atoms */
	for (size_t i = 0; i < frag->n_atoms; i++)
		efp_move_pt(CVEC(frag->x), &frag->rotmat,
			CVEC(frag->lib->atoms[i].x), VEC(frag->atoms[i].x));

	efp_update_elec(frag);
	efp_update_pol(frag);
	efp_update_disp(frag);
	efp_update_xr(frag);
}

static enum efp_result
set_coord_xyzabc(struct frag *frag, const double *coord)
{
	frag->x = coord[0];
	frag->y = coord[1];
	frag->z = coord[2];

	euler_to_matrix(coord[3], coord[4], coord[5], &frag->rotmat);

	update_fragment(frag);
	return EFP_RESULT_SUCCESS;
}

static enum efp_result
set_coord_points(struct frag *frag, const double *coord)
{
	if (frag->n_atoms < 3)
		return EFP_RESULT_NEED_THREE_ATOMS;

	double ref[9] = {
		frag->lib->atoms[0].x, frag->lib->atoms[0].y, frag->lib->atoms[0].z,
		frag->lib->atoms[1].x, frag->lib->atoms[1].y, frag->lib->atoms[1].z,
		frag->lib->atoms[2].x, frag->lib->atoms[2].y, frag->lib->atoms[2].z
	};

	vec_t p1;
	mat_t rot1, rot2;

	points_to_matrix(coord, &rot1);
	points_to_matrix(ref, &rot2);
	rot2 = mat_transpose(&rot2);
	frag->rotmat = mat_mat(&rot1, &rot2);
	p1 = mat_vec(&frag->rotmat, VEC(frag->lib->atoms[0].x));

	/* center of mass */
	frag->x = coord[0] - p1.x;
	frag->y = coord[1] - p1.y;
	frag->z = coord[2] - p1.z;

	update_fragment(frag);
	return EFP_RESULT_SUCCESS;
}

static enum efp_result
set_coord_rotmat(struct frag *frag, const double *coord)
{
	if (!check_rotation_matrix((const mat_t *)(coord + 3)))
		return EFP_RESULT_INVALID_ROTATION_MATRIX;

	frag->x = coord[0];
	frag->y = coord[1];
	frag->z = coord[2];

	memcpy(&frag->rotmat, coord + 3, sizeof(mat_t));

	update_fragment(frag);
	return EFP_RESULT_SUCCESS;
}

static void
free_frag(struct frag *frag)
{
	if (!frag)
		return;

	free(frag->atoms);
	free(frag->multipole_pts);
	free(frag->polarizable_pts);
	free(frag->dynamic_polarizable_pts);
	free(frag->lmo_centroids);
	free(frag->xr_fock_mat);
	free(frag->xr_wf);
	free(frag->screen_params);
	free(frag->ai_screen_params);

	for (size_t i = 0; i < 3; i++)
		free(frag->xr_wf_deriv[i]);

	for (size_t i = 0; i < frag->n_xr_shells; i++)
		free(frag->xr_shells[i].coef);

	free(frag->xr_shells);

	/* don't do free(frag) here */
}

static enum efp_result
copy_frag(struct frag *dest, const struct frag *src)
{
	size_t size;
	memcpy(dest, src, sizeof(struct frag));

	if (src->atoms) {
		size = src->n_atoms * sizeof(struct efp_atom);
		dest->atoms = malloc(size);
		if (!dest->atoms)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->atoms, src->atoms, size);
	}
	if (src->multipole_pts) {
		size = src->n_multipole_pts * sizeof(struct multipole_pt);
		dest->multipole_pts = malloc(size);
		if (!dest->multipole_pts)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->multipole_pts, src->multipole_pts, size);
	}
	if (src->screen_params) {
		size = src->n_multipole_pts * sizeof(double);
		dest->screen_params = malloc(size);
		if (!dest->screen_params)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->screen_params, src->screen_params, size);
	}
	if (src->ai_screen_params) {
		size = src->n_multipole_pts * sizeof(double);
		dest->ai_screen_params = malloc(size);
		if (!dest->ai_screen_params)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->ai_screen_params, src->ai_screen_params, size);
	}
	if (src->polarizable_pts) {
		size = src->n_polarizable_pts * sizeof(struct polarizable_pt);
		dest->polarizable_pts = malloc(size);
		if (!dest->polarizable_pts)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->polarizable_pts, src->polarizable_pts, size);
	}
	if (src->dynamic_polarizable_pts) {
		size = src->n_dynamic_polarizable_pts *
				sizeof(struct dynamic_polarizable_pt);
		dest->dynamic_polarizable_pts = malloc(size);
		if (!dest->dynamic_polarizable_pts)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->dynamic_polarizable_pts,
				src->dynamic_polarizable_pts, size);
	}
	if (src->lmo_centroids) {
		size = src->n_lmo * sizeof(vec_t);
		dest->lmo_centroids = malloc(size);
		if (!dest->lmo_centroids)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->lmo_centroids, src->lmo_centroids, size);
	}
	if (src->xr_shells) {
		size = src->n_xr_shells * sizeof(struct shell);
		dest->xr_shells = malloc(size);
		if (!dest->xr_shells)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->xr_shells, src->xr_shells, size);

		for (size_t i = 0; i < src->n_xr_shells; i++) {
			size = (src->xr_shells[i].type == 'L' ? 3 : 2) *
				src->xr_shells[i].n_funcs * sizeof(double);

			dest->xr_shells[i].coef = malloc(size);
			if (!dest->xr_shells[i].coef)
				return EFP_RESULT_NO_MEMORY;
			memcpy(dest->xr_shells[i].coef,
					src->xr_shells[i].coef, size);
		}
	}
	if (src->xr_fock_mat) {
		size = src->n_lmo * (src->n_lmo + 1) / 2 * sizeof(double);
		dest->xr_fock_mat = malloc(size);
		if (!dest->xr_fock_mat)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->xr_fock_mat, src->xr_fock_mat, size);
	}
	if (src->xr_wf) {
		size = src->n_lmo * src->xr_wf_size * sizeof(double);
		dest->xr_wf = malloc(size);
		if (!dest->xr_wf)
			return EFP_RESULT_NO_MEMORY;
		memcpy(dest->xr_wf, src->xr_wf, size);
	}
	return EFP_RESULT_SUCCESS;
}

static enum efp_result
check_opts(const struct efp_opts *opts)
{
	unsigned terms = opts->terms;

	if (((terms & EFP_TERM_AI_ELEC) && !(terms & EFP_TERM_ELEC)) ||
	    ((terms & EFP_TERM_AI_POL) && !(terms & EFP_TERM_POL)) ||
	    ((terms & EFP_TERM_POL) && !(terms & EFP_TERM_ELEC)) ||
	    ((terms & EFP_TERM_AI_DISP) && !(terms & EFP_TERM_DISP)) ||
	    ((terms & EFP_TERM_AI_XR) && !(terms & EFP_TERM_XR)) ||
	    ((terms & EFP_TERM_AI_CHTR) && !(terms & EFP_TERM_CHTR)))
		return EFP_RESULT_INCONSISTENT_TERMS;

	if (opts->enable_pbc) {
		if ((opts->terms & EFP_TERM_AI_ELEC) ||
		    (opts->terms & EFP_TERM_AI_POL) ||
		    (opts->terms & EFP_TERM_AI_DISP) ||
		    (opts->terms & EFP_TERM_AI_XR) ||
		    (opts->terms & EFP_TERM_AI_CHTR))
			return EFP_RESULT_PBC_NOT_SUPPORTED;

		if (opts->enable_links)
			return EFP_RESULT_PBC_NOT_SUPPORTED;

		if (!opts->enable_cutoff)
			return EFP_RESULT_PBC_REQUIRES_CUTOFF;
	}

	if (opts->enable_cutoff) {
		if (opts->swf_cutoff < 1.0)
			return EFP_RESULT_SWF_CUTOFF_TOO_SMALL;
	}

	return EFP_RESULT_SUCCESS;
}

static enum efp_result
check_frag_params(const struct efp_opts *opts, const struct frag *frag)
{
	if (opts->terms & EFP_TERM_ELEC) {
		if (!frag->multipole_pts)
			return EFP_RESULT_PARAMETERS_MISSING;

		if (opts->elec_damp == EFP_ELEC_DAMP_SCREEN && !frag->screen_params)
			return EFP_RESULT_PARAMETERS_MISSING;
	}
	if (opts->terms & EFP_TERM_POL) {
		if (!frag->polarizable_pts)
			return EFP_RESULT_PARAMETERS_MISSING;
	}
	if (opts->terms & EFP_TERM_DISP) {
		if (!frag->dynamic_polarizable_pts)
			return EFP_RESULT_PARAMETERS_MISSING;

		if (opts->disp_damp == EFP_DISP_DAMP_OVERLAP &&
		    frag->n_lmo != frag->n_dynamic_polarizable_pts)
			return EFP_RESULT_PARAMETERS_MISSING;
	}
	if (opts->terms & EFP_TERM_XR) {
		if (!frag->xr_shells ||
		    !frag->xr_fock_mat ||
		    !frag->xr_wf ||
		    !frag->lmo_centroids)
			return EFP_RESULT_PARAMETERS_MISSING;
	}
	return EFP_RESULT_SUCCESS;
}

static enum efp_result
check_params(struct efp *efp)
{
	enum efp_result res;

	for (size_t i = 0; i < efp->n_frag; i++)
		if ((res = check_frag_params(&efp->opts, efp->frags + i)))
			return res;

	return EFP_RESULT_SUCCESS;
}

static enum efp_result
setup_disp(struct efp *efp)
{
	efp->n_overlap = 0;

	if ((efp->opts.terms & EFP_TERM_DISP) == 0 ||
	    (efp->opts.disp_damp != EFP_DISP_DAMP_OVERLAP)) {
		free(efp->overlap_int);
		free(efp->overlap_int_deriv);
		efp->overlap_int = NULL;
		efp->overlap_int_deriv = NULL;
		return EFP_RESULT_SUCCESS;
	}

	for (size_t i = 0; i < efp->n_frag; i++) {
		efp->frags[i].overlap_offset = efp->n_overlap;

		for (size_t j = i + 1; j < efp->n_frag; j++)
			efp->n_overlap += efp->frags[i].n_lmo * efp->frags[j].n_lmo;
	}

	efp->overlap_int = realloc(efp->overlap_int,
			efp->n_overlap * sizeof(double));

	if (efp->overlap_int == NULL)
		return EFP_RESULT_NO_MEMORY;

	efp->overlap_int_deriv = realloc(efp->overlap_int_deriv,
			efp->n_overlap * sizeof(six_t));

	if (efp->overlap_int_deriv == NULL)
		return EFP_RESULT_NO_MEMORY;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_energy(struct efp *efp, struct efp_energy *energy)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!energy)
		return EFP_RESULT_INVALID_ARGUMENT;

	*energy = efp->energy;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_gradient(struct efp *efp, size_t n_frags, double *grad)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!grad)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!efp->do_gradient)
		return EFP_RESULT_GRADIENT_NOT_REQUESTED;

	if (n_frags != efp->n_frag)
		return EFP_RESULT_INVALID_ARRAY_SIZE;

	for (size_t i = 0; i < efp->n_frag; i++, grad += 6) {
		memcpy(grad, &efp->frags[i].force, sizeof(vec_t));
		memcpy(grad + 3, &efp->frags[i].torque, sizeof(vec_t));
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_point_charge_gradient(struct efp *efp, double *grad)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!grad)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!efp->do_gradient)
		return EFP_RESULT_GRADIENT_NOT_REQUESTED;

	for (size_t i = 0; i < efp->n_ptc; i++) {
		*grad++ = efp->point_charges[i].grad.x;
		*grad++ = efp->point_charges[i].grad.y;
		*grad++ = efp->point_charges[i].grad.z;
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_point_charge_count(struct efp *efp, size_t *n_ptc)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!n_ptc)
		return EFP_RESULT_INVALID_ARGUMENT;

	*n_ptc = efp->n_ptc;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_set_point_charges(struct efp *efp, size_t n_ptc,
			const double *q, const double *xyz)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (n_ptc == 0) {
		free(efp->point_charges);

		efp->n_ptc = 0;
		efp->point_charges = NULL;

		return EFP_RESULT_SUCCESS;
	}

	if (!q || !xyz)
		return EFP_RESULT_INVALID_ARGUMENT;

	efp->n_ptc = n_ptc;
	efp->point_charges = realloc(efp->point_charges,
				efp->n_ptc * sizeof(struct point_charge));

	if (!efp->point_charges)
		return EFP_RESULT_NO_MEMORY;

	for (size_t i = 0; i < efp->n_ptc; i++) {
		struct point_charge *ptc = efp->point_charges + i;

		ptc->x = *xyz++;
		ptc->y = *xyz++;
		ptc->z = *xyz++;

		ptc->charge = *q++;
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_point_charge_coordinates(struct efp *efp, double *xyz)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!xyz)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_ptc; i++) {
		struct point_charge *ptc = efp->point_charges + i;

		*xyz++ = ptc->x;
		*xyz++ = ptc->y;
		*xyz++ = ptc->z;
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_point_charge_values(struct efp *efp, double *q)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!q)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_ptc; i++)
		*q++ = efp->point_charges[i].charge;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_set_coordinates(struct efp *efp, enum efp_coord_type coord_type,
			const double *coord)
{
	enum efp_result res;
	size_t stride = (size_t []) {
		[EFP_COORD_TYPE_XYZABC] = 6,
		[EFP_COORD_TYPE_POINTS] = 9,
		[EFP_COORD_TYPE_ROTMAT] = 12 }[coord_type];

	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	for (size_t i = 0; i < efp->n_frag; i++, coord += stride)
		if ((res = efp_set_frag_coordinates(efp, i, coord_type, coord)))
			return res;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_set_frag_coordinates(struct efp *efp, size_t frag_idx,
	enum efp_coord_type coord_type, const double *coord)
{
	enum efp_result res;

	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	if (!coord)
		return EFP_RESULT_INVALID_ARGUMENT;

	struct frag *frag = efp->frags + frag_idx;

	switch (coord_type) {
		case EFP_COORD_TYPE_XYZABC:
			if ((res = set_coord_xyzabc(frag, coord)))
				return res;
			break;
		case EFP_COORD_TYPE_POINTS:
			if ((res = set_coord_points(frag, coord)))
				return res;
			break;
		case EFP_COORD_TYPE_ROTMAT:
			if ((res = set_coord_rotmat(frag, coord)))
				return res;
			break;
	}

	if (efp->opts.enable_links)
		update_ff_atoms(efp->ff, frag);

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_coordinates(struct efp *efp, size_t n_frags, double *xyzabc)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!xyzabc)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (n_frags != efp->n_frag)
		return EFP_RESULT_INVALID_ARRAY_SIZE;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		double a, b, c;
		matrix_to_euler(&frag->rotmat, &a, &b, &c);

		*xyzabc++ = frag->x;
		*xyzabc++ = frag->y;
		*xyzabc++ = frag->z;
		*xyzabc++ = a;
		*xyzabc++ = b;
		*xyzabc++ = c;
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_set_periodic_box(struct efp *efp, double x, double y, double z)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (x < 2.0 * efp->opts.swf_cutoff ||
	    y < 2.0 * efp->opts.swf_cutoff ||
	    z < 2.0 * efp->opts.swf_cutoff)
		return EFP_RESULT_BOX_TOO_SMALL;

	efp->box.x = x;
	efp->box.y = y;
	efp->box.z = z;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_stress_tensor(struct efp *efp, double *stress)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!stress)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!efp->do_gradient)
		return EFP_RESULT_GRADIENT_NOT_REQUESTED;

	*(mat_t *)stress = efp->stress;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_wavefunction_dependent_energy(struct efp *efp, double *energy)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!energy)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!(efp->opts.terms & EFP_TERM_POL)) {
		*energy = 0.0;
		return EFP_RESULT_SUCCESS;
	}

	return efp_compute_pol_energy(efp, energy);
}

EFP_EXPORT enum efp_result
efp_compute(struct efp *efp, int do_gradient)
{
	typedef enum efp_result (*term_fn)(struct efp *);

	static const term_fn term_list[] = {
		efp_compute_xr,   /* xr must be first */
		efp_compute_elec,
		efp_compute_pol,
		efp_compute_disp,
		efp_compute_ai_elec
	};

	enum efp_result res;

	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	efp->do_gradient = do_gradient;

	if ((res = check_params(efp)))
		return res;

	if ((res = setup_disp(efp)))
		return res;

	efp->stress = mat_zero;
	memset(&efp->energy, 0, sizeof(struct efp_energy));

	if (efp->overlap_int)
		memset(efp->overlap_int, 0, sizeof(double) * efp->n_overlap);

	if (efp->overlap_int_deriv)
		memset(efp->overlap_int_deriv, 0, sizeof(six_t) * efp->n_overlap);

	for (size_t i = 0; i < efp->n_frag; i++) {
		efp->frags[i].force = vec_zero;
		efp->frags[i].torque = vec_zero;
	}

	for (size_t i = 0; i < efp->n_ptc; i++)
		efp->point_charges[i].grad = vec_zero;

	for (size_t i = 0; i < ARRAY_SIZE(term_list); i++)
		if ((res = term_list[i](efp)))
			return res;

#ifdef WITH_MPI
	vec_t grad_f[2 * efp->n_frag];

	for (size_t i = 0; i < efp->n_frag; i++) {
		grad_f[i] = efp->frags[i].force;
		grad_f[efp->n_frag + i] = efp->frags[i].torque;
	}

	MPI_Allreduce(MPI_IN_PLACE, grad_f, 6 * (int)efp->n_frag, MPI_DOUBLE,
			MPI_SUM, MPI_COMM_WORLD);

	for (size_t i = 0; i < efp->n_frag; i++) {
		efp->frags[i].force = grad_f[i];
		efp->frags[i].torque = grad_f[efp->n_frag + i];
	}

	vec_t grad_p[efp->n_ptc];

	for (size_t i = 0; i < efp->n_ptc; i++)
		grad_p[i] = efp->point_charges[i].grad;

	MPI_Allreduce(MPI_IN_PLACE, grad_p, 3 * (int)efp->n_ptc, MPI_DOUBLE,
			MPI_SUM, MPI_COMM_WORLD);

	for (size_t i = 0; i < efp->n_ptc; i++)
		efp->point_charges[i].grad = grad_p[i];

	MPI_Allreduce(MPI_IN_PLACE, &efp->stress, 9, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
#endif

	if (efp->opts.enable_links) {
		size_t n_ff_atoms = efp_ff_get_atom_count(efp->ff);
		vec_t ff_grad[n_ff_atoms];

		efp->energy.covalent = efp_ff_compute(efp->ff, ff_grad);

		if (do_gradient) {
			for (size_t i = 0; i < efp->n_frag; i++) {
				struct frag *frag = efp->frags + i;
				add_ff_gradient(frag, ff_grad + frag->ff_offset);
			}
		}
	}

	efp->energy.total = efp->energy.electrostatic +
			    efp->energy.charge_penetration +
			    efp->energy.electrostatic_point_charges +
			    efp->energy.polarization +
			    efp->energy.dispersion +
			    efp->energy.exchange_repulsion +
			    efp->energy.covalent;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_charge(struct efp *efp, size_t frag_idx, double *charge)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	if (!charge)
		return EFP_RESULT_INVALID_ARGUMENT;

	struct frag *frag = efp->frags + frag_idx;
	*charge = 0.0;

	for (size_t i = 0; i < frag->n_atoms; i++)
		*charge += frag->atoms[i].znuc;

	for (size_t i = 0; i < frag->n_multipole_pts; i++)
		*charge += frag->multipole_pts[i].monopole;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_multiplicity(struct efp *efp, size_t frag_idx, int *mult)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	if (!mult)
		return EFP_RESULT_INVALID_ARGUMENT;

	*mult = efp->frags[frag_idx].multiplicity;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_multipole_count(struct efp *efp, size_t *n_mult)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!n_mult)
		return EFP_RESULT_INVALID_ARGUMENT;

	size_t sum = 0;

	for (size_t i = 0; i < efp->n_frag; i++)
		sum += efp->frags[i].n_multipole_pts;

	*n_mult = sum;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_multipole_coordinates(struct efp *efp, double *xyz)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!xyz)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		for (size_t j = 0; j < frag->n_multipole_pts; j++) {
			*xyz++ = frag->multipole_pts[j].x;
			*xyz++ = frag->multipole_pts[j].y;
			*xyz++ = frag->multipole_pts[j].z;
		}
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_multipole_values(struct efp *efp, double *mult)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!mult)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		for (size_t j = 0; j < frag->n_multipole_pts; j++) {
			struct multipole_pt *pt = frag->multipole_pts + j;

			*mult++ = pt->monopole;

			*mult++ = pt->dipole.x;
			*mult++ = pt->dipole.y;
			*mult++ = pt->dipole.z;

			for (size_t t = 0; t < 6; t++)
				*mult++ = pt->quadrupole[t];

			for (size_t t = 0; t < 10; t++)
				*mult++ = pt->octupole[t];
		}
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_induced_dipole_count(struct efp *efp, size_t *n_dip)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!n_dip)
		return EFP_RESULT_INVALID_ARGUMENT;

	size_t sum = 0;

	for (size_t i = 0; i < efp->n_frag; i++)
		sum += efp->frags[i].n_polarizable_pts;

	*n_dip = sum;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_induced_dipole_coordinates(struct efp *efp, double *xyz)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!xyz)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		for (size_t j = 0; j < frag->n_polarizable_pts; j++) {
			struct polarizable_pt *pt = frag->polarizable_pts + j;

			*xyz++ = pt->x;
			*xyz++ = pt->y;
			*xyz++ = pt->z;
		}
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_induced_dipole_values(struct efp *efp, double *dip)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!dip)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		for (size_t j = 0; j < frag->n_polarizable_pts; j++) {
			struct polarizable_pt *pt = frag->polarizable_pts + j;

			*dip++ = pt->induced_dipole.x;
			*dip++ = pt->induced_dipole.y;
			*dip++ = pt->induced_dipole.z;
		}
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_induced_dipole_conj_values(struct efp *efp, double *dip)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!dip)
		return EFP_RESULT_INVALID_ARGUMENT;

	for (size_t i = 0; i < efp->n_frag; i++) {
		struct frag *frag = efp->frags + i;

		for (size_t j = 0; j < frag->n_polarizable_pts; j++) {
			struct polarizable_pt *pt = frag->polarizable_pts + j;

			*dip++ = pt->induced_dipole_conj.x;
			*dip++ = pt->induced_dipole_conj.y;
			*dip++ = pt->induced_dipole_conj.z;
		}
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT void
efp_shutdown(struct efp *efp)
{
	if (!efp)
		return;

	for (size_t i = 0; i < efp->n_frag; i++)
		free_frag(efp->frags + i);

	for (size_t i = 0; i < efp->n_lib; i++) {
		free_frag(efp->lib[i]);
		free(efp->lib[i]);
	}

	free(efp->frags);
	free(efp->lib);
	free(efp->point_charges);
	efp_ff_free(efp->ff);
	efp_bvec_free(efp->links_bvec);
	free(efp);
}

EFP_EXPORT enum efp_result
efp_set_opts(struct efp *efp, const struct efp_opts *opts)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!opts)
		return EFP_RESULT_INVALID_ARGUMENT;

	enum efp_result res;

	if ((res = check_opts(opts)))
		return res;

	efp->opts = *opts;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_opts(struct efp *efp, struct efp_opts *opts)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!opts)
		return EFP_RESULT_INVALID_ARGUMENT;

	*opts = efp->opts;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT void
efp_opts_default(struct efp_opts *opts)
{
	assert(opts);

	memset(opts, 0, sizeof(struct efp_opts));

	opts->terms = EFP_TERM_ELEC | EFP_TERM_POL | EFP_TERM_DISP |
		EFP_TERM_XR | EFP_TERM_AI_ELEC | EFP_TERM_AI_POL;
}

EFP_EXPORT enum efp_result
efp_add_fragment(struct efp *efp, const char *name)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!name)
		return EFP_RESULT_INVALID_ARGUMENT;

	enum efp_result res;
	const struct frag *lib = efp_find_lib(efp, name);

	if (!lib)
		return EFP_RESULT_UNKNOWN_FRAGMENT;

	efp->n_frag++;
	efp->frags = realloc(efp->frags, efp->n_frag * sizeof(struct frag));

	if (!efp->frags)
		return EFP_RESULT_NO_MEMORY;

	struct frag *frag = efp->frags + efp->n_frag - 1;

	if ((res = copy_frag(frag, lib)))
		return res;

	for (size_t a = 0; a < 3; a++) {
		size_t size = frag->xr_wf_size * frag->n_lmo;

		frag->xr_wf_deriv[a] = malloc(size * sizeof(double));

		if (!frag->xr_wf_deriv[a])
			return EFP_RESULT_NO_MEMORY;
	}

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_load_forcefield(struct efp *efp, const char *path)
{
	enum ff_res ff_res;

	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!path)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!efp->opts.enable_links)
		return EFP_RESULT_LINKS_ARE_NOT_ENABLED;

	if ((ff_res = efp_ff_parse(efp->ff, path)))
		return ff_to_efp(ff_res);

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_load_topology(struct efp *efp, const char *path)
{
	enum ff_res ff_res;
	enum efp_result res;

	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!path)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (!efp->opts.enable_links)
		return EFP_RESULT_LINKS_ARE_NOT_ENABLED;

	if ((efp->links_bvec = efp_bvec_create(efp->n_frag * efp->n_frag)) == NULL)
		return EFP_RESULT_NO_MEMORY;

	if ((res = init_ff(efp)))
		return res;

	if ((res = parse_topology(efp, path)))
		return res;

	if ((ff_res = efp_ff_auto_angles(efp->ff)))
		return ff_to_efp(ff_res);

	if ((ff_res = efp_ff_auto_torsions(efp->ff)))
		return ff_to_efp(ff_res);

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT struct efp *
efp_create(void)
{
	struct efp *efp = calloc(1, sizeof(struct efp));

	if (!efp)
		return NULL;

	efp->ff = efp_ff_create();

	if (!efp->ff) {
		free(efp);
		return NULL;
	}

	efp_opts_default(&efp->opts);

	return efp;
}

EFP_EXPORT enum efp_result
efp_set_electron_density_field_fn(struct efp *efp, efp_electron_density_field_fn fn)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	efp->get_electron_density_field = fn;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_set_electron_density_field_user_data(struct efp *efp, void *user_data)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	efp->get_electron_density_field_user_data = user_data;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_count(struct efp *efp, size_t *n_frag)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!n_frag)
		return EFP_RESULT_INVALID_ARGUMENT;

	*n_frag = efp->n_frag;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_name(struct efp *efp, size_t frag_idx, size_t size, char *frag_name)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!frag_name)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	if ((unsigned)size <= strlen(efp->frags[frag_idx].name))
		return EFP_RESULT_INVALID_ARRAY_SIZE;

	strcpy(frag_name, efp->frags[frag_idx].name);
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_mass(struct efp *efp, size_t frag_idx, double *mass_out)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!mass_out)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	const struct frag *frag = efp->frags + frag_idx;
	double mass = 0.0;

	for (size_t i = 0; i < frag->n_atoms; i++)
		mass += frag->atoms[i].mass;

	*mass_out = mass;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_inertia(struct efp *efp, size_t frag_idx, double *inertia_out)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!inertia_out)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	/* center of mass is in origin and axes are principal axes of inertia */

	const struct frag *frag = efp->frags[frag_idx].lib;
	vec_t inertia = vec_zero;

	for (size_t i = 0; i < frag->n_atoms; i++) {
		const struct efp_atom *atom = frag->atoms + i;

		inertia.x += atom->mass * (atom->y * atom->y + atom->z * atom->z);
		inertia.y += atom->mass * (atom->x * atom->x + atom->z * atom->z);
		inertia.z += atom->mass * (atom->x * atom->x + atom->y * atom->y);
	}

	inertia_out[0] = inertia.x;
	inertia_out[1] = inertia.y;
	inertia_out[2] = inertia.z;

	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_atom_count(struct efp *efp, size_t frag_idx, size_t *n_atoms)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!n_atoms)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	*n_atoms = efp->frags[frag_idx].n_atoms;
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT enum efp_result
efp_get_frag_atoms(struct efp *efp, size_t frag_idx,
		   size_t size, struct efp_atom *atoms)
{
	if (!efp)
		return EFP_RESULT_NOT_INITIALIZED;

	if (!atoms)
		return EFP_RESULT_INVALID_ARGUMENT;

	if (frag_idx >= efp->n_frag)
		return EFP_RESULT_INDEX_OUT_OF_RANGE;

	struct frag *frag = efp->frags + frag_idx;

	if (size < frag->n_atoms)
		return EFP_RESULT_INVALID_ARRAY_SIZE;

	memcpy(atoms, frag->atoms, frag->n_atoms * sizeof(struct efp_atom));
	return EFP_RESULT_SUCCESS;
}

EFP_EXPORT void
efp_torque_to_derivative(const double *euler, const double *torque, double *deriv)
{
	assert(euler);
	assert(torque);
	assert(deriv);

	double tx = torque[0];
	double ty = torque[1];
	double tz = torque[2];

	double sina = sin(euler[0]);
	double cosa = cos(euler[0]);
	double sinb = sin(euler[1]);
	double cosb = cos(euler[1]);

	deriv[0] = tz;
	deriv[1] = cosa * tx + sina * ty;
	deriv[2] = sinb * sina * tx - sinb * cosa * ty + cosb * tz;
}

EFP_EXPORT const char *
efp_banner(void)
{
	static const char banner[] =
		"LIBEFP ver. " LIBEFP_VERSION_STRING "\n"
		"Copyright (c) 2012-2013 Ilya Kaliman\n"
		"Project web site: http://www.libefp.org/\n";

	return banner;
}

EFP_EXPORT const char *
efp_result_to_string(enum efp_result res)
{
	switch (res) {
	case EFP_RESULT_SUCCESS:
return "no error";
	case EFP_RESULT_NO_MEMORY:
return "out of memory";
	case EFP_RESULT_INVALID_ARGUMENT:
return "invalid argument to function was specified";
	case EFP_RESULT_NOT_INITIALIZED:
return "structure was not properly initialized";
	case EFP_RESULT_FILE_NOT_FOUND:
return "EFP potential data file not found";
	case EFP_RESULT_SYNTAX_ERROR:
return "syntax error in potential data";
	case EFP_RESULT_UNKNOWN_FRAGMENT:
return "unknown EFP fragment type";
	case EFP_RESULT_DUPLICATE_PARAMETERS:
return "fragment parameters contain fragments with the same name";
	case EFP_RESULT_CALLBACK_NOT_SET:
return "required callback function is not set";
	case EFP_RESULT_CALLBACK_FAILED:
return "callback function failed";
	case EFP_RESULT_GRADIENT_NOT_REQUESTED:
return "gradient computation was not requested";
	case EFP_RESULT_PBC_NOT_SUPPORTED:
return "periodic simulation is not supported for selected energy terms";
	case EFP_RESULT_PBC_REQUIRES_CUTOFF:
return "interaction cutoff must be enabled for periodic simulation";
	case EFP_RESULT_SWF_CUTOFF_TOO_SMALL:
return "switching function cutoff is too small";
	case EFP_RESULT_BOX_TOO_SMALL:
return "periodic simulation box is too small";
	case EFP_RESULT_NEED_THREE_ATOMS:
return "fragment must contain at least three atoms";
	case EFP_RESULT_POL_NOT_CONVERGED:
return "polarization SCF did not converge";
	case EFP_RESULT_PARAMETERS_MISSING:
return "required EFP fragment parameters are missing";
	case EFP_RESULT_INVALID_ROTATION_MATRIX:
return "invalid rotation matrix specified";
	case EFP_RESULT_INDEX_OUT_OF_RANGE:
return "index is out of range";
	case EFP_RESULT_INVALID_ARRAY_SIZE:
return "invalid array size";
	case EFP_RESULT_UNKNOWN_FF_TYPE:
return "unknown force field atom type";
	case EFP_RESULT_LINKS_ARE_NOT_ENABLED:
return "covalent fragment-fragment links are not enabled";
	case EFP_RESULT_UNKNOWN_ATOM:
return "unknown atom name";
	case EFP_RESULT_UNSUPPORTED_SCREEN:
return "unsupported SCREEN group found in EFP data";
	case EFP_RESULT_INCONSISTENT_TERMS:
return "inconsistent EFP energy terms selected";
	}
	assert(0);
}
