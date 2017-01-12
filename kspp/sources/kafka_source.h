#include <memory>
#include <strstream>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <kspp/kspp.h>
#include "kafka_consumer.h"
#pragma once

#define LOGPREFIX_ERROR BOOST_LOG_TRIVIAL(error) << BOOST_CURRENT_FUNCTION << name()

namespace kspp {
  template<class K, class V, class CODEC>
  class kafka_source_base : public partition_source<K, V>
  {
  public:
    virtual ~kafka_source_base() {
      close();
    }

    virtual void start(int64_t offset) {
      _consumer.start(offset);
    }

    //TBD get offsets from kafka
    virtual void start() {
      _consumer.start(-2);
    }

    virtual void close() {
      _consumer.close();
    }

    virtual inline bool eof() const {
      return _consumer.eof();
    }

    // TBD lazy commit offsets to kafka
    virtual void commit() {}

    // TBD hard commit offsets to kafka
    virtual void flush_offset() {}

    virtual bool is_dirty() {
      return !_consumer.eof();
    }

    virtual bool process_one() {
      auto p = _consumer.consume();
      if (!p)
        return false;
      send_to_sinks(parse(p));
      return true;
    }

  protected:
    kafka_source_base(std::string brokers, std::string topic, size_t partition, std::shared_ptr<CODEC> codec)
      : partition_source<K, V>(NULL, partition)
      , _codec(codec)
      , _consumer(brokers, topic, partition) {}

    virtual std::shared_ptr<krecord<K, V>> parse(const std::unique_ptr<RdKafka::Message> & ref) = 0;

    kafka_consumer          _consumer;
    std::shared_ptr<CODEC>  _codec;
  };

  template<class K, class V, class CODEC>
  class kafka_source : public kafka_source_base<K, V, CODEC>
  {
  public:
    kafka_source(std::string brokers, std::string topic, size_t partition, std::shared_ptr<CODEC> codec = std::make_shared<CODEC>())
      : kafka_source_base<K, V, CODEC>(brokers, topic, partition, codec) {}

    virtual std::string name() const {
      return "kafka_source-" + kafka_source_base::_consumer.topic() + "-" + std::to_string(kafka_source_base::_consumer.partition());
    }

  protected:
    std::shared_ptr<krecord<K, V>> parse(const std::unique_ptr<RdKafka::Message> & ref) {
      if (!ref)
        return NULL;

      auto res = std::make_shared<krecord<K, V>>();

      res->event_time = ref->timestamp().timestamp;
      res->offset = ref->offset();
      {
        std::istrstream ks((const char*)ref->key_pointer(), ref->key_len());
        size_t consumed = _codec->decode(ks, res->key);
        if (consumed == 0) {
          LOGPREFIX_ERROR << ", decode key failed, actual key sz:" << ref->key_len();
          return NULL;
        } else if (consumed != ref->key_len()) {
          LOGPREFIX_ERROR << ", decode key failed, consumed: " << consumed << ", actual: " << ref->key_len();
          return NULL;
        }
      }

      size_t sz = ref->len();
      if (sz) {
        std::istrstream vs((const char*)ref->payload(), sz);
        res->value = std::make_shared<V>();
        size_t consumed = _codec->decode(vs, *res->value);
        if (consumed == 0) {
          LOGPREFIX_ERROR << ", decode value failed, size:" << sz;
          return NULL;
        } else if (consumed != sz) {
          LOGPREFIX_ERROR << ", decode value failed, consumed: " << consumed << ", actual: " << sz;
          return NULL;
        }
      }
      return res;
    }
  };

  // <void, VALUE>
  template<class V, class CODEC>
  class kafka_source<void, V, CODEC> : public kafka_source_base<void, V, CODEC>
  {
  public:
    kafka_source(std::string brokers, std::string topic, size_t partition, std::shared_ptr<CODEC> codec = std::make_shared<CODEC>())
      : kafka_source_base<void, V, CODEC>(brokers, topic, partition, codec) {}

    virtual std::string name() const {
      return "kafka_source<void, K>-" + kafka_source_base<void, V, CODEC>::_consumer.topic() + "-" + std::to_string(kafka_source_base<void, V, CODEC>::_consumer.partition());
    }

  protected:
    std::shared_ptr<krecord<void, V>> parse(const std::unique_ptr<RdKafka::Message> & ref) {
      if (!ref)
        return NULL;
      size_t sz = ref->len();
      if (sz) {
        std::istrstream vs((const char*)ref->payload(), sz);
        auto v = std::make_shared<V>();
        size_t consumed = _codec->decode(vs, *v);
        if (consumed == sz) {
          auto res = std::make_shared<krecord<void, V>>(v);
          res->event_time = ref->timestamp().timestamp;
          res->offset = ref->offset();
          return res;
        }

        if (consumed == 0) {
          LOGPREFIX_ERROR << ", decode value failed, size:" << sz;
          return NULL;
        }

        LOGPREFIX_ERROR << ", decode value failed, consumed: " << consumed << ", actual: " << sz;
        return NULL;
      }
      return NULL; // just parsed an empty message???
    }

  };


  //<KEY, NULL>
  template<class K, class CODEC>
  class kafka_source<K, void, CODEC> : public kafka_source_base<K, void, CODEC>
  {
  public:
  public:
    kafka_source(std::string brokers, std::string topic, size_t partition, std::shared_ptr<CODEC> codec = std::make_shared<CODEC>())
      : kafka_source_base<K, void, CODEC>(brokers, topic, partition, codec) {}

    virtual std::string name() const {
      return "kafka_source<K, void>-" + kafka_source_base<K, void, CODEC>::_consumer.topic() + "-" + std::to_string(kafka_source_base<K, void, CODEC>::_consumer.partition());
    }

  protected:
    std::shared_ptr<krecord<K, void>> parse(const std::unique_ptr<RdKafka::Message> & ref) {
      if (!ref || ref->key_len() == 0)
        return NULL;

      auto res = std::make_shared<krecord<K, void>>();

      res->event_time = ref->timestamp().timestamp;
      res->offset = ref->offset();
      std::istrstream ks((const char*)ref->key_pointer(), ref->key_len());
      size_t consumed = _codec->decode(ks, res->key);
      if (consumed == 0) {
        LOGPREFIX_ERROR << ", decode key failed, actual key sz:" << ref->key_len();
        return NULL;
      } else if (consumed != ref->key_len()) {
        LOGPREFIX_ERROR << ", decode key failed, consumed: " << consumed << ", actual: " << ref->key_len();
        return NULL;
      }
      return res;
    }
  };
};

