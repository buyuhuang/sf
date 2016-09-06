#include <iostream>
#include <iomanip>
#include <cstdint>
#include <sstream>
#include <string>

#include <Rcpp.h>

#define SF_Point               1
#define SF_LineString          2
#define SF_Polygon             3
#define SF_MultiPoint          4
#define SF_MultiLineString     5
#define SF_MultiPolygon        6
#define SF_GeometryCollection  7
#define SF_CircularString      8
#define SF_CompoundCurve       9
#define SF_CurvePolygon       10
#define SF_MultiCurve         11
#define SF_MultiSurface       12
#define SF_Curve              13
#define SF_Surface            14
#define SF_PolyhedralSurface  15
#define SF_TIN                16
#define SF_Triangle           17

#define EWKB_Z_BIT             0x80000000
#define EWKB_M_BIT             0x40000000
#define EWKB_SRID_BIT          0x20000000

// using namespace Rcpp;

Rcpp::NumericMatrix ReadMultiPoint(unsigned char **pt, int n_dims, bool EWKB, int endian, 
	Rcpp::CharacterVector cls, uint32_t *srid);
Rcpp::NumericVector ReadNumericVector(unsigned char **pt, int n, 
	Rcpp::CharacterVector cls, uint32_t *srid);
Rcpp::NumericMatrix ReadNumericMatrix(unsigned char **pt, int n_dims, 
	Rcpp::CharacterVector cls, uint32_t *srid);
Rcpp::List ReadMatrixList(unsigned char **pt, int n_dims, 
	Rcpp::CharacterVector cls, uint32_t *srid);
Rcpp::List ReadGC(unsigned char **pt, int n_dims, bool EWKB, int endian, 
	Rcpp::CharacterVector cls, bool addclass, uint32_t *srid);
Rcpp::List ReadData(unsigned char **pt, bool EWKB, int endian, bool debug, 
	bool addclass, int *type);
Rcpp::NumericVector GetBBOX(Rcpp::List sf, int depth);
void WriteData(std::ostringstream& os, Rcpp::List sfc, int i, bool EWKB, int endian,
	bool debug, const char *cls, const char *dim);
unsigned int mkType(const char *cls, const char *dim, bool EWKB, int *tp);

// [[Rcpp::export]]
Rcpp::List HexToRaw(Rcpp::CharacterVector cx) {
// HexToRaw modified from cmhh, see https://github.com/ianmcook/wkb/issues/10
// @cmhh: if you make yourself known, I can add you to the contributors

// convert a hexadecimal string into a raw vector

	Rcpp::List output(cx.size());
	Rcpp::CharacterVector invec(cx);
	for (int j=0; j<cx.size(); j++) {
		Rcpp::RawVector raw(invec[j].size() / 2);
		std::string s = Rcpp::as<std::string>(invec[j]);
		int x;
		for (int i=0; i<raw.size(); i++){
			std::istringstream iss(s.substr(i*2, 2));
			iss >> std::hex >> x;
			raw[i] = x;
			if (i % 100000 == 0)
				Rcpp::checkUserInterrupt();
		}
		output[j] = raw;
		if (j % 1000 == 0)
			Rcpp::checkUserInterrupt();
	}
	return output;
}


// [[Rcpp::export]]
Rcpp::List ReadWKB(Rcpp::List wkb_list, bool EWKB = false, int endian = 0, bool debug = false) {
	Rcpp::List output(wkb_list.size());

	int type = 0, last_type = 0, n_types = 0;

	for (int i = 0; i < wkb_list.size(); i++) {
		Rcpp::checkUserInterrupt();
		Rcpp::RawVector raw = wkb_list[i];
		unsigned char *pt = &(raw[0]);
		output[i] = ReadData(&pt, EWKB, endian, debug, true, &type)[0];
		if (type != last_type) {
			last_type = type;
			n_types++;
		}
	}
	output.attr("n_types") = n_types; // if this is 1, we can skip the coerceTypes later on
	return output;
}

Rcpp::List ReadData(unsigned char **pt, bool EWKB = false, int endian = 0, 
		bool debug = false, bool addclass = true, int *type = NULL) {

	Rcpp::List output(1); // to make result type opaque
	// do endian check, only support native endian WKB:
	if ((int) (**pt) != (int) endian)
		throw std::range_error("non native endian: use pureR = TRUE"); // life is too short
	(*pt)++;
	// read type:
	uint32_t *wkbType = (uint32_t *) (*pt); // uint32_t requires -std=c++11
	(*pt) += 4;
	uint32_t *srid = NULL;
	// Rprintf("[%u]\n", *wkbType);
	int sf_type = 0, dim = 0, n_dims = 0;
	std::string dim_str = ""; 
	if (EWKB) { // EWKB: PostGIS default
		sf_type =     *wkbType & 0x000000ff; // mask the other bits
		int wkbZ =    *wkbType & EWKB_Z_BIT;
		int wkbM =    *wkbType & EWKB_M_BIT;
		int wkbSRID = *wkbType & EWKB_SRID_BIT;
		n_dims = 2 + (int) (wkbZ != 0) + (int) (wkbM != 0);
		if (wkbZ == 0 && wkbM == 0)
			dim_str = "XY";
		else if (wkbZ != 0 && wkbM == 0)
			dim_str = "XYZ";
		else if (wkbZ == 0 && wkbM != 1)
			dim_str = "XYM";
		else
			dim_str = "XYZM";
		if (wkbSRID != 0) {
			srid = (uint32_t *) (*pt);
			(*pt) += 4;
		}
	} else { // ISO
		sf_type = *wkbType % 1000;
		switch (*wkbType / 1000) { // 0: XY, 1: XYZ, 2: XYM, 3: XYZM
			case 0: n_dims = 2; dim_str = "XY"; break; 
			case 1: n_dims = 3; dim_str = "XYZ"; break; 
			case 2: n_dims = 3; dim_str = "XYM"; break; 
			case 3: n_dims = 4; dim_str = "XYZM"; break; 
			default:
				throw std::range_error("unknown wkbType dim in switch");
		}
	}
	if (debug) {
		Rcpp::Rcout << "sf_type: " << sf_type << std::endl;
		Rcpp::Rcout << "n_dims:  " << n_dims << std::endl;
		Rcpp::Rcout << "dim_str: " << dim_str << std::endl;
		if (srid != NULL)
			Rcpp::Rcout << "srid: NA" << std::endl;
		else
			Rcpp::Rcout << "srid: " <<  *srid << std::endl;
	}
	switch(sf_type) {
		case SF_Point: 
			output[0] = ReadNumericVector(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "POINT", "sfi") : "", srid);
			break;
		case SF_LineString:
			output[0] = ReadNumericMatrix(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "LINESTRING", "sfi") : "", srid); 
			break;
		case SF_Polygon: 
			output[0] = ReadMatrixList(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "POLYGON", "sfi") : "", srid);
			break;
		case SF_MultiPoint: 
			output[0] = ReadMultiPoint(pt, n_dims, EWKB, endian, addclass ?
				Rcpp::CharacterVector::create(dim_str, "MULTIPOINT", "sfi") : "", srid); 
			break;
		case SF_MultiLineString:
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "MULTILINESTRING", "sfi"), false, srid);
			break;
		case SF_MultiPolygon:
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "MULTIPOLYGON", "sfi"), false, srid);
			break;
		case SF_GeometryCollection: 
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "GEOMETRYCOLLECTION", "sfi"), true, srid);
			break;
		case SF_CircularString:
			output[0] = ReadNumericMatrix(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "CIRCULARSTRING", "sfi") : "", srid); 
			break;
		case SF_MultiCurve:
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "MULTICURVE", "sfi"), false, srid);
			break;
		case SF_MultiSurface:
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "MULTISURFACE", "sfi"), false, srid);
			break;
		case SF_Curve:
			output[0] = ReadNumericMatrix(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "CURVE", "sfi") : "", srid); 
			break;
		case SF_Surface: 
			output[0] = ReadMatrixList(pt, n_dims, addclass ?
				Rcpp::CharacterVector::create(dim_str, "SURFACE", "sfi") : "", srid);
			break;
		case SF_PolyhedralSurface: 
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "POLYHEDRALSURFACE", "sfi"), false, srid);
			break;
		case SF_TIN: 
			output[0] = ReadGC(pt, n_dims, EWKB, endian,
				Rcpp::CharacterVector::create(dim_str, "TIN", "sfi"), false, srid);
			break;
		case SF_Triangle:
			output[0] = ReadMatrixList(pt, n_dims,
				Rcpp::CharacterVector::create(dim_str, "TRIANGLE", "sfi"), srid);
			break;
		default: {
			Rcpp::Rcout << "type is " << sf_type << "\n";
			throw std::range_error("reading this sf type is not supported, please file an issue");
		}
	}
	if (type != NULL)
		*type = sf_type;
	return(output);
}

Rcpp::NumericMatrix ReadMultiPoint(unsigned char **pt, int n_dims, bool EWKB = 0, int endian = 0, 
		Rcpp::CharacterVector cls = "", uint32_t *srid = NULL) {
	uint32_t *npts = (uint32_t *) (*pt); // requires -std=c++11
	(*pt) += 4;
	Rcpp::NumericMatrix ret(*npts, n_dims);
	for (int i = 0; i < *npts; i++) {
		Rcpp::List lst = ReadData(pt, EWKB, endian);
		Rcpp::NumericVector vec = lst[0];
		for (int j = 0; j < n_dims; j++)
			ret(i,j) = vec(j);
	}
	if (cls.size() == 3) {
		ret.attr("class") = cls;
		if (srid != NULL)
			ret.attr("epsg") = (int) *srid;
	}
	return(ret);
}

Rcpp::List ReadGC(unsigned char **pt, int n_dims, bool EWKB = 0, int endian = 0, 
		Rcpp::CharacterVector cls = "", bool isGC = true, uint32_t *srid = NULL) {
	uint32_t *nlst = (uint32_t *) (*pt); // requires -std=c++11
	(*pt) += 4;
	Rcpp::List ret(*nlst);
	for (int i = 0; i < *nlst; i++)
		ret[i] = ReadData(pt, EWKB, endian, false, isGC)[0];
	if (cls.size() == 3) {
		ret.attr("class") = cls;
		if (srid != NULL)
			ret.attr("epsg") = (int) *srid;
	}
	return(ret);
}

Rcpp::NumericVector ReadNumericVector(unsigned char **pt, int n, 
		Rcpp::CharacterVector cls = "", uint32_t *srid = NULL) {
	Rcpp::NumericVector ret(n);
	double *d = (double *) (*pt);
	for (int i=0; i<n; i++)
		ret(i) = *d++;
	(*pt) = (unsigned char *) d;
	if (cls.size() == 3) {
		ret.attr("class") = cls;
		if (srid != NULL)
			ret.attr("epsg") = (int) *srid;
	}
	return ret;
}

Rcpp::NumericMatrix ReadNumericMatrix(unsigned char **pt, int n_dims, 
		Rcpp::CharacterVector cls = "", uint32_t *srid = NULL) {
	uint32_t *npts = (uint32_t *) (*pt); // requires -std=c++11
	(*pt) += 4;
	Rcpp::NumericMatrix ret(*npts, n_dims);
	double *d = (double *) (*pt);
	for (int i=0; i<(*npts); i++)
		for (int j=0; j<n_dims; j++)
			ret(i,j) = *d++;
	(*pt) = (unsigned char *) d;
	if (cls.size() == 3) {
		ret.attr("class") = cls;
		if (srid != NULL)
			ret.attr("epsg") = (int) *srid;
	}
	return ret;
}

Rcpp::List ReadMatrixList(unsigned char **pt, int n_dims, 
		Rcpp::CharacterVector cls = "", uint32_t *srid = NULL) {
	uint32_t *nlst = (uint32_t *) (*pt); // requires -std=c++11
	(*pt) += 4;
	Rcpp::List ret(*nlst);
	for (int i = 0; i < (*nlst); i++)
		ret[i] = ReadNumericMatrix(pt, n_dims, "");
	if (cls.size() == 3) {
		ret.attr("class") = cls;
		if (srid != NULL)
			ret.attr("epsg") = (int) *srid;
	}
	return(ret);
}

//
// WriteWKB:
//

// [[Rcpp::export]]
Rcpp::List WriteWKB(Rcpp::List sfc, bool EWKB = false, int endian = 0, 
		Rcpp::CharacterVector dim = "XY", bool debug = false) {

	Rcpp::List output(sfc.size()); // with raw vectors
	int type = 0, last_type = 0, n_types = 0;
	Rcpp::CharacterVector cls_attr = sfc.attr("class");
	const char *cls = cls_attr[0], *dm = dim[0];
	if (debug) 
		Rcpp::Rcout << dm << std::endl;

	for (int i = 0; i < sfc.size(); i++) {
		std::ostringstream os;
		Rcpp::checkUserInterrupt();
		WriteData(os, sfc, i, EWKB, endian, debug, cls, dm);
		Rcpp::RawVector raw(os.str().size()); // os -> raw:
		std::string str = os.str();
		const char *cp = str.c_str();
		for (int j = 0; j < str.size(); j++)
			raw[j] = cp[j];
		output[i] = raw; // raw vector to list
	}
	return output;
}

unsigned int mkType(const char *cls, const char *dim, bool EWKB = false, int *tp = NULL) {
	int type = 0;
	if (strstr(cls, "sfc_") == cls)
		cls += 4;
	// Rcpp::Rcout << cls << " " << dim << std::endl;
	if (strcmp(cls, "POINT") == 0)
		type = SF_Point;
	else if (strcmp(cls, "LINESTRING") == 0)
		type = SF_LineString;
	else if (strcmp(cls, "POLYGON") == 0)
		type = SF_Polygon;
	else if (strcmp(cls, "MULTIPOINT") == 0)
		type = SF_MultiPoint;
	else if (strcmp(cls, "MULTILINESTRING") == 0)
		type = SF_MultiLineString;
	else if (strcmp(cls, "MULTIPOLYGON") == 0)
		type = SF_MultiPolygon;
	else if (strcmp(cls, "GEOMETRYCOLLECTION") == 0)
		type = SF_GeometryCollection;
	else if (strcmp(cls, "CIRCULARSTRING") == 0)
		type = SF_CircularString;
	else if (strcmp(cls, "COMPOUNDCURVE") == 0)
		type = SF_CompoundCurve;
	else if (strcmp(cls, "CURVEPOLYGON") == 0)
		type = SF_CurvePolygon;
	else if (strcmp(cls, "MULTISURFACE") == 0)
		type = SF_MultiSurface;
	else if (strcmp(cls, "CURVE") == 0)
		type = SF_Curve;
	else if (strcmp(cls, "SURFACE") == 0)
		type = SF_Surface;
	else if (strcmp(cls, "POLYHEDRALSURFACE") == 0)
		type = SF_PolyhedralSurface;
	else if (strcmp(cls, "TIN") == 0)
		type = SF_TIN;
	else if (strcmp(cls, "TRIANGLE") == 0)
		type = SF_Triangle;
	else
		throw std::range_error("unknown type!");
	*tp = type;
	if (EWKB) {
		if (strcmp(dim, "XYZ") == 0)
			type = type | EWKB_Z_BIT;
		else if (strcmp(dim, "XYM") == 0)
			type = type | EWKB_M_BIT;
		else if (strcmp(dim, "XYZM") == 0)
			type = type | EWKB_M_BIT | EWKB_Z_BIT;
	} else {
		if (strcmp(dim, "XYZ") == 0)
			type += 1000;
		else if (strcmp(dim, "XYM") == 0)
			type += 2000;
		else if (strcmp(dim, "XYZM") == 0)
			type += 3000;
	}
	return(type);
}

void addByte(std::ostringstream& os, char c) {
  os.write((char*) &c, sizeof(char));
}

void addInt(std::ostringstream& os, unsigned int i) {
  const char *cp = (char *)&i;
  os.write((char*) cp, sizeof(int));
}

void addDouble(std::ostringstream& os, double d) {
  const char *cp = (char *)&d;
  os.write((char*) cp, sizeof(double));
}

void WriteVector(std::ostringstream& os, Rcpp::NumericVector vec) {
	for (unsigned int i = 0; i < vec.length(); i++)
		addDouble(os, vec(i));
}

void WriteMatrix(std::ostringstream& os, Rcpp::NumericMatrix mat) {
	addInt(os, mat.nrow());
	for (unsigned int i = 0; i < mat.nrow(); i++)
		for (unsigned int j = 0; j < mat.ncol(); j++)
			addDouble(os, mat(i,j));
}

void WriteMatrixList(std::ostringstream& os, Rcpp::List lst) {
	unsigned int len = lst.length();
	addInt(os, len);
	for (unsigned int i = 0; i < len; i++)
		WriteMatrix(os, lst[i]);
}

void WriteMultiLineString(std::ostringstream& os, Rcpp::List lst, bool EWKB = false, int endian = 0) {
	Rcpp::CharacterVector cl_attr = lst.attr("class");
	const char *dim = cl_attr[0];
	addInt(os, lst.length());
	for (int i = 0; i < lst.length(); i++)
		WriteData(os, lst, i, EWKB, endian, false, "LINESTRING", dim);
}

void WriteMultiPolygon(std::ostringstream& os, Rcpp::List lst, bool EWKB = false, int endian = 0) {
	Rcpp::CharacterVector cl_attr = lst.attr("class");
	const char *dim = cl_attr[0];
	addInt(os, lst.length());
	for (int i = 0; i < lst.length(); i++)
		WriteData(os, lst, i, EWKB, endian, false, "POLYGON", dim);
}

void WriteGC(std::ostringstream& os, Rcpp::List lst, bool EWKB = false, int endian = 0) {
	addInt(os, lst.length());
	Rcpp::Function Rclass("class");
	for (int i = 0; i < lst.length(); i++) {
		Rcpp::CharacterVector cl_attr = Rclass(lst[i]); 
		const char *cls = cl_attr[1], *dim = cl_attr[0];
		WriteData(os, lst, i, EWKB, endian, false, cls, dim);
	}
}

void WriteMultiPoint(std::ostringstream& os, Rcpp::NumericMatrix mat, 
		bool EWKB = false, int endian = 0) {
	addInt(os, mat.nrow());
	Rcpp::CharacterVector cl_attr = mat.attr("class");
	const char *dim = cl_attr[0];
	Rcpp::NumericVector v(mat.ncol()); // copy row i
	Rcpp::List lst(1);
	for (int i = 0; i < mat.nrow(); i++) {
		for (int j = 0; j < mat.ncol(); j++)
			v(j) = mat(i,j);
		lst[0] = v;
		WriteData(os, lst, 0, EWKB, endian, false, "POINT", dim);
	}
}

// write single simple feature object as WKB to stream os
void WriteData(std::ostringstream& os, Rcpp::List sfc, int i = 0, bool EWKB = false, 
		int endian = 0, bool debug = false, const char *cls = NULL, const char *dim = NULL) {
	
	addByte(os, (char) endian);
	int tp;
	unsigned int sf_type = mkType(cls, dim, EWKB, &tp);
	if (debug)
		Rcpp::Rcout << "sf_type:" << sf_type << std::endl;
	addInt(os, sf_type);
	switch(tp) {
		case SF_Point: WriteVector(os, sfc[i]);
			break;
		case SF_LineString: WriteMatrix(os, sfc[i]);
			break;
		case SF_Polygon: WriteMatrixList(os, sfc[i]);
			break;
		case SF_MultiPoint: WriteMultiPoint(os, sfc[i], EWKB, endian);
			break;
		case SF_MultiLineString: WriteMultiLineString(os, sfc[i], EWKB, endian);
			break;
		case SF_MultiPolygon: WriteMultiPolygon(os, sfc[i], EWKB, endian);
			break;
		case SF_GeometryCollection: WriteGC(os, sfc[i], EWKB, endian);
			break;
		case SF_CircularString: WriteMatrix(os, sfc[i]);
			break;
		case SF_MultiCurve: WriteGC(os, sfc[i], EWKB, endian);
			break;
		case SF_MultiSurface: WriteGC(os, sfc[i], EWKB, endian);
			break;
		case SF_Curve: WriteMatrix(os, sfc[i]);
			break;
		case SF_Surface: WriteMatrixList(os, sfc[i]);
			break;
		case SF_PolyhedralSurface: WriteMultiPolygon(os, sfc[i], EWKB, endian);
			break;
		case SF_TIN: WriteMultiPolygon(os, sfc[i], EWKB, endian);
			break;
		case SF_Triangle: WriteMatrixList(os, sfc[i]);
			break;
		default: {
			Rcpp::Rcout << "type is " << sf_type << "\n";
			throw std::range_error("writing this sf type is not supported, please file an issue");
		}
	}
}