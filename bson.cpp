// bson.cpp
/**
 *  Copyright 2009 10gen, Inc.
 * 
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

// note: key size is limited to 256 characters

#include <php.h>
#include <mongo/client/dbclient.h>

#include "mongo_id.h"
#include "mongo_date.h"
#include "mongo_regex.h"
#include "mongo_bindata.h"

extern zend_class_entry *mongo_id_class;
extern zend_class_entry *mongo_date_class;
extern zend_class_entry *mongo_regex_class;
extern zend_class_entry *mongo_bindata_class;

int php_array_to_bson( mongo::BSONObjBuilder *obj_builder, HashTable *arr_hash ) {
  zval **data;
  char *key;
  uint key_len;
  ulong index;
  zend_bool duplicate = 0;
  HashPosition pointer;
  int num = 0;

  if( zend_hash_num_elements( arr_hash ) == 0 ) {
    return num;
  }

  for(zend_hash_internal_pointer_reset_ex(arr_hash, &pointer); 
      zend_hash_get_current_data_ex(arr_hash, (void**) &data, &pointer) == SUCCESS; 
      zend_hash_move_forward_ex(arr_hash, &pointer)) {
    num++;
    int key_type = zend_hash_get_current_key_ex(arr_hash, &key, &key_len, &index, duplicate, &pointer);
    char field_name[256];

    // make \0 safe
    if( key_type == HASH_KEY_IS_STRING ) {
      strcpy( field_name, key );
    }
    else if( key_type == HASH_KEY_IS_LONG ) {
      sprintf( field_name, "%ld", index );
    }
    else {
      zend_error( E_ERROR, "key fail" );
      break;
    }

    switch (Z_TYPE_PP(data)) {
    case IS_NULL:
      obj_builder->appendNull( field_name );
      break;
    case IS_LONG:
      obj_builder->append( field_name, (int)Z_LVAL_PP(data) );
      break;
    case IS_DOUBLE:
      obj_builder->append( field_name, Z_DVAL_PP(data) );
      break;
    case IS_BOOL:
      obj_builder->append( field_name, Z_BVAL_PP(data) );
      break;
    case IS_ARRAY: {
      mongo::BSONObjBuilder *subobj = new mongo::BSONObjBuilder();
      php_array_to_bson( subobj, Z_ARRVAL_PP( data ) );
      obj_builder->append( field_name, subobj->done() );
      break;
    }
    // this should probably be done as bin data, to guard against \0
    case IS_STRING:
      obj_builder->append( field_name, Z_STRVAL_PP(data) );
      break;
    case IS_OBJECT: {
      TSRMLS_FETCH();
      zend_class_entry *clazz = Z_OBJCE_PP( data );
      if(clazz == mongo_id_class) {
        zval *zid = zend_read_property( mongo_id_class, *data, "id", 2, 0 TSRMLS_CC );
        char *cid = Z_STRVAL_P( zid );
        std::string *id = new string( cid );
        mongo::OID *oid = new mongo::OID();
        oid->init( *id );

        obj_builder->appendOID( field_name, oid ); 
      }
      else if (clazz == mongo_date_class) {
        zval *zsec = zend_read_property( mongo_date_class, *data, "sec", 3, 0 TSRMLS_CC );
        long sec = Z_LVAL_P( zsec );
        zval *zusec = zend_read_property( mongo_date_class, *data, "usec", 4, 0 TSRMLS_CC );
        long usec = Z_LVAL_P( zusec );
        unsigned long long d = (unsigned long long)(sec * 1000) + (unsigned long long)(usec/1000);

        obj_builder->appendDate(field_name, d); 
      }
      else if (clazz == mongo_regex_class) {
        zval *zre = zend_read_property( mongo_regex_class, *data, "regex", 5, 0 TSRMLS_CC );
        char *re = Z_STRVAL_P( zre );
        zval *zflags = zend_read_property( mongo_regex_class, *data, "flags", 5, 0 TSRMLS_CC );
        char *flags = Z_STRVAL_P( zflags );

        obj_builder->appendRegex(field_name, re, flags); 
      }
      else if (clazz == mongo_bindata_class) {
        zval *zbin = zend_read_property( mongo_bindata_class, *data, "bin", 3, 0 TSRMLS_CC );
        char *bin = Z_STRVAL_P( zbin );
        zval *zlen = zend_read_property( mongo_bindata_class, *data, "length", 6, 0 TSRMLS_CC );
        long len = Z_LVAL_P( zlen );
        zval *ztype = zend_read_property( mongo_bindata_class, *data, "type", 4, 0 TSRMLS_CC );
        long type = Z_LVAL_P( ztype );

        switch(type) {
        case 1:
          obj_builder->appendBinData(field_name, len, mongo::Function, bin); 
          break;
        case 3:
          obj_builder->appendBinData(field_name, len, mongo::bdtUUID, bin); 
          break;
        case 5:
          obj_builder->appendBinData(field_name, len, mongo::MD5Type, bin); 
          break;
        case 128:
          obj_builder->appendBinData(field_name, len, mongo::bdtCustom, bin); 
          break;
        default:
          obj_builder->appendBinData(field_name, len, mongo::ByteArray, bin); 
          break;
        }
      }
      break;
    }
    case IS_RESOURCE:
    case IS_CONSTANT:
    case IS_CONSTANT_ARRAY:
    default:
      php_printf( "php=>bson: type %i not supported", Z_TYPE_PP(data) );
    }
  }

  return num;
}

zval *bson_to_php_array( mongo::BSONObj obj ) {
  zval *array;
  ALLOC_INIT_ZVAL( array );
  array_init(array);

  mongo::BSONObjIterator it = mongo::BSONObjIterator( obj );
  while( it.more() ) {
    mongo::BSONElement elem = it.next();

    char *key = (char*)elem.fieldName();
    int index = atoi( key );
    // check if 0 index is valid, or just a failed 
    // string conversion
    if( index == 0 && strcmp( "0", key ) != 0 ) {
      index = -1;
    }
    int assoc = index == -1;

    switch( elem.type() ) {
    case mongo::Undefined:
    case mongo::jstNULL: {
      if( assoc )
        add_assoc_null( array, key );
      else 
        add_index_null( array, index );
      break;
    }
    case mongo::NumberInt: {
      long num = (long)elem.number();
      if( assoc )
        add_assoc_long( array, key, num );
      else 
        add_index_long( array, index, num );
      break;
    }
    case mongo::NumberDouble: {
      double num = elem.number();
      if( assoc )
        add_assoc_double( array, key, num );
      else 
        add_index_double( array, index, num );
      break;
    }
    case mongo::Bool: {
      int b = elem.boolean();
      if( assoc )
        add_assoc_bool( array, key, b );
      else 
        add_index_bool( array, index, b );
      break;
    }
    case mongo::String: {
      char *value = (char*)elem.valuestr();
      if( assoc ) 
        add_assoc_string( array, key, value, 1 );
      else 
        add_index_string( array, index, value, 1 );
      break;
    }
    case mongo::Date: {
      zval *zdate = date_to_mongo_date( elem.date() );
      if( assoc ) 
        add_assoc_zval( array, key, zdate );
      else 
        add_index_zval( array, index, zdate );
      break;
    }
    case mongo::RegEx: {
      zval *zre = re_to_mongo_re((char*)elem.regex(), (char*)elem.regexFlags());
      if( assoc ) 
        add_assoc_zval( array, key, zre );
      else 
        add_index_zval( array, index, zre );
      break;
    }
    case mongo::BinData: {
      int size;
      char *bin = (char*)elem.binData(size);
      int type = elem.binDataType();
      zval *phpbin = bin_to_php_bin(bin, size, type);
      if( assoc ) 
        add_assoc_zval( array, key, phpbin );
      else 
        add_index_zval( array, index, phpbin );
      break;
    }
    case mongo::Array:
    case mongo::Object: {
      zval *subarray = bson_to_php_array( elem.embeddedObject() );
      if( assoc ) 
        add_assoc_zval( array, key, subarray );
      else 
        add_index_zval( array, index, subarray );
      break;
    }
    case mongo::jstOID: {
      zval *zoid = oid_to_mongo_id( elem.__oid() );
      if( assoc ) 
        add_assoc_zval( array, key, zoid );
      else 
        add_index_zval( array, index, zoid );
      break;
    }
    case mongo::EOO: {
      break;
    }
    default:
      php_printf( "bson=>php: type %i not supported\n", elem.type() );
    }
  }
  return array;
}

void prep_obj_for_db( mongo::BSONObjBuilder *array ) {
  mongo::OID *oid = new mongo::OID();
  oid->init();
  array->appendOID( "_id", oid);
}