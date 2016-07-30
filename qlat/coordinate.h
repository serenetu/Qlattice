#pragma once

#include <qlat/config.h>

#include <array>
#include <sstream>
#include <iostream>

QLAT_START_NAMESPACE

struct Coordinate: public std::array<int, DIM>
{
        Coordinate(){
                std::array<int, DIM>::fill(0);
        }

        Coordinate(int first, int second, int third, int fourth){
                int *p = data();
                p[0] = first;
                p[1] = second;
                p[2] = third;
                p[3] = fourth;
        }

        int product() const {
                int ret = 1;
		int size_ = size();
                for(int i = 0; i < size_; i++){
                        ret *= operator[](i);
                }
                return ret;
        }
};

Coordinate operator*(int integer, const Coordinate &coor)
{
	return Coordinate(integer * coor[0], integer * coor[1],
				integer * coor[2], integer * coor[3]);
}

Coordinate operator-(const Coordinate &coor1, const Coordinate &coor2)
{
	return Coordinate(coor1[0] - coor2[0], coor1[1] - coor2[1], \
				coor1[2] - coor2[2], coor1[3] - coor2[3]);
}

Coordinate operator+(const Coordinate &coor1, const Coordinate &coor2)
{
	return Coordinate(coor1[0] + coor2[0], coor1[1] + coor2[1], \
				coor1[2] + coor2[2], coor1[3] + coor2[3]);
}

void regularize(Coordinate &coor, const Coordinate &regularizer)
{
	for(int mu = 0; mu < DIM; mu++){
	coor[mu] = (coor[mu] % regularizer[mu] + regularizer[mu]) % regularizer[mu];
	}
}

template<class CharT, class Traits>
std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, 
                   const Coordinate coor){
	std::basic_ostringstream<CharT, Traits> os_;
	os_.flags(os.flags());
	os_.imbue(os.getloc());
	os_.precision(os.precision());
	os_ << "(" << coor[0] << ", "
		<< coor[1] << ", "
		<< coor[2] << ", "
		<< coor[3] << ")";
	return os << os_.str();
}

struct CoordinateLess
{
	bool operator()(const Coordinate &coor1, const Coordinate &coor2) const {
		return operator<(coor1, coor2);
	}
};

QLAT_END_NAMESPACE


