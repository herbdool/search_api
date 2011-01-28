<?php
// $Id$

/**
 * Class representing a search index.
 */
class SearchApiIndex extends Entity {

  /**
   * Cached return value of server().
   *
   * @var SearchApiServer
   */
  protected $server_object = NULL;

  /**
   * @var array
   */
  protected $callbacks = NULL;

  /**
   * @var array
   */
  protected $processors = NULL;

  /**
   * @var array
   */
  protected $added_properties = NULL;

  /**
   * @var array
   */
  protected $fulltext_fields = array();

  // Database values that will be set when object is loaded

  /**
   * @var integer
   */
  public $id;

  /**
   * @var string
   */
  public $name;

  /**
   * @var string
   */
  public $machine_name;

  /**
   * @var string
   */
  public $description;

  /**
   * @var string
   */
  public $server;

  /**
   * @var string
   */
  public $entity_type;

  /**
   * @var array
   */
  public $options;

  /**
   * @var integer
   */
  public $enabled;

  /**
   * Constructor as a helper to the parent constructor.
   */
  public function __construct(array $values = array()) {
    parent::__construct($values, 'search_api_index');
  }

  /**
   * Execute necessary tasks for a newly created index (either created in the
   * database, or for the first time loaded from code).
   */
  public function postCreate() {
    // Remember items to index.
    $entity_info = entity_get_info($this->entity_type);
    if (!empty($entity_info['base table'])) {
      // Use a subselect, which will probably be much faster than entity_load().
      // We just assume that no module/entity type will be stupid enough to use "base table" and
      // "entity_keys[id]" in a different way than the default controller.
      $id_field = $entity_info['entity keys']['id'];
      $table = $entity_info['base table'];
      $query = db_select($table, 't');
      $query->addField('t', $id_field, 'item_id');
      $query->addExpression(':index_id', 'index_id', array(':index_id' => $this->id));
      $query->addExpression('1', 'changed');

      db_insert('search_api_item')->from($query)->execute();
    }
    else {
      // We have to use the slow entity_load().
      $entities = entity_load($this->entity_type, FALSE);
      $query = db_insert('search_api_item')->fields(array('item_id', 'index_id', 'changed'));
      foreach ($entities as $item_id => $entity) {
        $query->values(array(
          'item_id' => $item_id,
          'index_id' => $this->id,
          'changed' => 1,
        ));
      }
      $query->execute();
    }

    $server = $this->server();
    if ($server) {
      // Tell the server about the new index.
      if ($server->enabled) {
        $server->addIndex($this);
      }
      else {
        $tasks = variable_get('search_api_tasks', array());
        // When we add or remove an index, we can ignore all other tasks.
        $tasks[$server->machine_name][$this->machine_name] = array('add');
        variable_set('search_api_tasks', $tasks);
      }
    }
  }

  /**
   * Execute necessary tasks when index is either deleted from the database or
   * not defined in code anymore.
   */
  public function postDelete() {
    if ($server = $this->server()) {
      if ($server->enabled) {
        $server->removeIndex($this);
      }
      else {
        $tasks = variable_get('search_api_tasks', array());
        $tasks[$server->machine_name][$this->machine_name] = array('remove');
        variable_set('search_api_tasks', $tasks);
      }
    }

    db_delete('search_api_item')
      ->condition('index_id', $this->id)
      ->execute();
  }

  /**
   * Saves this index to the database, either creating a new record or updating
   * an existing one.
   *
   * @return
   *   Failure to save the index will return FALSE. Otherwise, SAVED_NEW or
   *   SAVED_UPDATED is returned depending on the operation performed. $this->id
   *   will be set if a new index was inserted.
   */
  public function save() {
    if (empty($this->description)) {
      $this->description = NULL;
    }
    if (empty($this->server)) {
      $this->server = NULL;
      $this->enabled = FALSE;
    }
    // This will also throw an exception if the server doesn't exist – which is good.
    elseif (!$this->server(TRUE)->enabled) {
      $this->enabled = FALSE;
    }

    return parent::save();
  }

  /**
   * Helper method for updating entity properties.
   *
   * NOTE: You shouldn't change any properties of this object before calling
   * this method, as this might lead to the fields not being saved correctly.
   *
   * @param array $fields
   *   The new field values.
   *
   * @return
   *   SAVE_UPDATED on success, FALSE on failure, 0 if the fields already had
   *   the specified values.
   */
  public function update(array $fields) {
    $changeable = array('name' => 1, 'enabled' => 1, 'description' => 1, 'server' => 1, 'options' => 1);
    $changed = FALSE;
    foreach ($fields as $field => $value) {
      if (isset($changeable[$field]) && $value !== $this->$field) {
        $this->$field = $value;
        $changed = TRUE;
      }
    }
    // If there are no new values, just return 0.
    if (!$changed) {
      return 0;
    }
    return $this->save();
  }

  /**
   * Schedules this search index for re-indexing.
   *
   * @return
   *   TRUE on success, FALSE on failure.
   */
  public function reindex() {
    if (!$this->server) {
      return TRUE;
    }
    $ret = _search_api_index_reindex($this->id);
    if($ret) {
      module_invoke_all('search_api_index_reindex', $this, FALSE);
    }
    return TRUE;
  }

  /**
   * Clears this search index and schedules all of its items for re-indexing.
   *
   * @return
   *   TRUE on success, FALSE on failure.
   */
  public function clear() {
    if (!$this->server) {
      return TRUE;
    }

    $server = $this->server();
    if ($server->enabled) {
      $server->deleteItems('all', $this);
    }
    else {
      $tasks = variable_get('search_api_tasks', array());
      // If the index was cleared or newly added since the server was last enabled, we don't need to do anything.
      if (!isset($tasks[$server->machine_name][$this->id])
          || (array_search('add', $tasks[$server->machine_name][$this->id]) === FALSE
              && array_search('clear', $tasks[$server->machine_name][$this->id]) === FALSE)) {
        $tasks[$server->machine_name][$this->id][] = 'clear';
        variable_set('search_api_tasks', $tasks);
      }
    }

    $ret = _search_api_index_reindex($this->id);
    if($ret) {
      module_invoke_all('search_api_index_reindex', $this, TRUE);
    }

    return TRUE;
  }

  /**
   * Magic method for determining which fields should be serialized.
   *
   * Don't serialize properties that are basically only caches.
   *
   * @return array
   *   An array of properties to be serialized.
   */
  public function __sleep() {
    $ret = get_object_vars($this);
    unset($ret['server_object'], $ret['processors'], $ret['added_properties'], $ret['fulltext_fields'], $ret['status'], $ret['module'], $ret['is_new']);
    return array_keys($ret);
  }

  /**
   * Get the server this index lies on.
   *
   * @param $reset
   *   Whether to reset the internal cache. Set to TRUE when the index' $server
   *   property has just changed.
   *
   * @throws SearchApiException
   *   If $this->server is set, but no server with that machine name exists.
   *
   * @return SearchApiServer
   *   The server associated with this index, or NULL if this index currently
   *   doesn't lie on a server.
   */
  public function server($reset = FALSE) {
    if (!isset($this->server_object) || $reset) {
      $this->server_object = $this->server ? search_api_server_load($this->server) : FALSE;
      if ($this->server && !$this->server_object) {
        throw new SearchApiException(t('Unknown server !server specified for index !name.',
            array('!server' => $this->server, '!name' => $this->machine_name)));
      }
    }
    return $this->server_object ? $this->server_object : NULL;
  }

  /**
   * Create a query object for this index.
   *
   * @param $options
   *   Associative array of options configuring this query. See
   *   SearchApiQueryInterface::__construct().
   *
   * @throws SearchApiException
   *   If the index is currently disabled.
   *
   * @return SearchApiQueryInterface
   *   A query object for searching this index.
   */
  public function query($options = array()) {
    if (!$this->enabled) {
      throw new SearchApiException(t('Cannot search on a disabled index.'));
    }
    return $this->server()->query($this, $options);
  }


  /**
   * Indexes items on this index. Will return an array of IDs of items that
   * should be marked as indexed – i.e. items that were either rejected by a
   * data-alter callback or were successfully indexed.
   *
   * @param array $items
   *   An array of entities to index.
   *
   * @return array
   *   An array of the IDs of all items that should be marked as indexed.
   */
  public function index(array $items) {
    if (!$this->enabled) {
      throw new SearchApiException(t("Couldn't index values on '!name' index (index is disabled)", array('!name' => $this->name)));
    }
    if (empty($this->options['fields'])) {
      throw new SearchApiException(t("Couldn't index values on '!name' index (no fields selected)", array('!name' => $this->name)));
    }

    $fields = $this->options['fields'];
    foreach ($fields as $field => $info) {
      if (!$info['indexed']) {
        unset($fields[$field]);
      }
      unset($fields[$field]['indexed']);
    }
    if (empty($fields)) {
      throw new SearchApiException(t("Couldn't index values on '!name' index (no fields selected)", array('!name' => $this->name)));
    }

    // Mark all items that are rejected by a data-alter callback as indexed.
    $ret = array_keys($items);
    $this->dataAlter($items);
    $ret = array_diff($ret, array_keys($items));

    // Items that are rejected should also be deleted from the server.
    if ($ret) {
      $this->server()->deleteItems($ret, $this);
    }

    if (!$items) {
      watchdog('search_api', t('Indexing the items of type "!type" with the following IDs was rejected by data-alter callbacks: !list.', array('type' => $this->entity_type, '!list' => implode(', ', $ret))));
      return $ret;
    }
    $wrappers = array();
    foreach ($items as $id => $item) {
      $wrappers[$id] = $this->entityWrapper($item);
    }

    $items = array();
    foreach ($wrappers as $id => $wrapper) {
      $items[$id] = search_api_extract_fields($wrapper, $fields);
    }

    $this->preprocessIndexItems($items);

    return array_merge($ret, $this->server()->indexItems($this, $items));
  }

  /**
   * Calls data alteration hooks for a set of items, according to the index
   * options.
   *
   * @param array $items
   *   An array of items to be altered.
   *
   * @return SearchApiIndex
   *   The called object.
   */
  public function dataAlter(array &$items) {
    // First, execute our own search_api_language data alteration.
    foreach ($items as &$item) {
      $item->search_api_language = isset($item->language) ? $item->language : LANGUAGE_NONE;
    }

    foreach ($this->getAlterCallbacks() as $callback) {
      $callback->alterItems($items);
    }

    return $this;
  }

  /**
   * Property info alter callback that adds the infos of the properties added by
   * data alter callbacks.
   *
   * @param EntityMetadataWrapper $wrapper
   *   The wrapped data.
   * @param $property_info
   *   The original property info.
   *
   * @return array
   *   The altered property info.
   */
  public function propertyInfoAlter(EntityMetadataWrapper $wrapper, array $property_info) {
    // Overwrite the existing properties with the list of properties including
    // all fields regardless of the used bundle.
    $property_info['properties'] = entity_get_all_property_info($wrapper->type());

    if (!isset($this->added_properties)) {
      $this->added_properties = array(
        'search_api_language' => array(
          'label' => t('Item language'),
          'description' => t("A field added by the search framework to let components determine an item's language. Is always indexed."),
          'type' => 'string',
        ),
      );
      // We use the reverse order here so the hierarchy for overwriting property infos is the same
      // as for actually overwriting the properties.
      foreach (array_reverse($this->getAlterCallbacks()) as $callback) {
        $props = $callback->propertyInfo();
        if ($props) {
          $this->added_properties += $props;
        }
      }
    }
    // Let fields added by data-alter callbacks override default fields.
    $property_info['properties'] = $this->added_properties + $property_info['properties'];

    return $property_info;
  }

   /**
   * Fills the $processors array for use by the pre-/postprocessing functions.
   *
   * @return SearchApiIndex
   *   The called object.
   * @return array
   *   All enabled callbacks for this index, as SearchApiAlterCallbackInterface
   *   objects.
   */
  protected function getAlterCallbacks() {
    if (isset($this->callbacks)) {
      return $this->callbacks;
    }

    $this->callbacks = array();
    if (empty($this->options['data_alter_callbacks'])) {
      return $this->callbacks;
    }
    $callback_settings = $this->options['data_alter_callbacks'];
    $infos = search_api_get_alter_callbacks();

    foreach ($callback_settings as $id => $settings) {
      if (empty($settings['status'])) {
        continue;
      }
      if (empty($infos[$id]) || !class_exists($infos[$id]['class'])) {
        watchdog('search_api', t('Undefined data alteration !class specified in index !name', array('!class' => $id, '!name' => $this->name)), NULL, WATCHDOG_WARNING);
        continue;
      }
      $class = $infos[$id]['class'];
      $callback = new $class($this, empty($settings['settings']) ? array() : $settings['settings']);
      if (!($callback instanceof SearchApiAlterCallbackInterface)) {
        watchdog('search_api', t('Unknown callback class !class specified for data alteration !name', array('!class' => $class, '!name' => $id)), NULL, WATCHDOG_WARNING);
        continue;
      }

      $this->callbacks[$id] = $callback;
    }
    return $this->callbacks;
  }

  /**
   * @return array
   *   All enabled processors for this index, as SearchApiProcessorInterface
   *   objects.
   */
  protected function getProcessors() {
    if (isset($this->processors)) {
      return $this->processors;
    }

    $this->processors = array();
    if (empty($this->options['processors'])) {
      return $this->processors;
    }
    $processor_settings = $this->options['processors'];
    $infos = search_api_get_processors();

    foreach ($processor_settings as $id => $settings) {
      if (empty($settings['status'])) {
        continue;
      }
      if (empty($infos[$id]) || !class_exists($infos[$id]['class'])) {
        watchdog('search_api', t('Undefined processor !class specified in index !name', array('!class' => $id, '!name' => $this->name)), NULL, WATCHDOG_WARNING);
        continue;
      }
      $class = $infos[$id]['class'];
      $processor = new $class($this, isset($settings['settings']) ? $settings['settings'] : array());
      if (!($processor instanceof SearchApiProcessorInterface)) {
        watchdog('search_api', t('Unknown processor class !class specified for processor !name', array('!class' => $class, '!name' => $id)), NULL, WATCHDOG_WARNING);
        continue;
      }

      $this->processors[$id] = $processor;
    }
    return $this->processors;
  }

  /**
   * Preprocess data items for indexing. Data added by data alter callbacks will
   * be available on the items.
   *
   * Typically, a preprocessor will execute its preprocessing (e.g. stemming,
   * n-grams, word splitting, stripping stop words, etc.) only on the items'
   * fulltext fields. Other fields should usually be left untouched.
   *
   * @param array $items
   *   An array of items to be preprocessed for indexing.
   *
   * @return SearchApiIndex
   *   The called object.
   */
  public function preprocessIndexItems(array &$items) {
    foreach ($this->getProcessors() as $processor) {
      $processor->preprocessIndexItems($items);
    }
    return $this;
  }


  /**
   * Preprocess a search query.
   *
   * The same applies as when preprocessing indexed items: typically, only the
   * fulltext search keys should be processed, queries on specific fields should
   * usually not be altered.
   *
   * @param SearchApiQuery $query
   *   The object representing the query to be executed.
   *
   * @return SearchApiIndex
   *   The called object.
   */
  public function preprocessSearchQuery(SearchApiQuery $query) {
    foreach ($this->getProcessors() as $processor) {
      $processor->preprocessSearchQuery($query);
    }
    return $this;
  }

  /**
   * Postprocess search results before display.
   *
   * If a class is used for both pre- and post-processing a search query, the
   * same object will be used for both calls (so preserving some data or state
   * locally is possible).
   *
   * @param array $response
   *   An array containing the search results. See
   *   SearchApiServiceInterface->search() for the detailled format.
   * @param SearchApiQuery $query
   *   The object representing the executed query.
   *
   * @return SearchApiIndex
   *   The called object.
   */
  public function postprocessSearchResults(array &$response, SearchApiQuery $query) {
    // Postprocessing is done in exactly the opposite direction than preprocessing.
    foreach (array_reverse($this->getProcessors()) as $processor) {
      $processor->postprocessSearchResults($response, $query);
    }
    return $this;
  }

  /**
   * Convenience method for getting all of this index' fulltext fields.
   *
   * @param boolean $only_indexed
   *   If set to TRUE, only the indexed fulltext fields will be returned.
   *
   * @return array
   *   An array containing all (or all indexed) fulltext fields defined for this
   *   index.
   */
  public function getFulltextFields($only_indexed = TRUE) {
    $i = $only_indexed ? 1 : 0;
    if (!isset($this->fulltext_fields[$i])) {
      $this->fulltext_fields[$i] = array();
      if (empty($this->options['fields'])) {
        return array();
      }
      foreach ($this->options['fields'] as $key => $field) {
        if (search_api_is_text_type($field['type']) && (!$only_indexed || $field['indexed'])) {
          $this->fulltext_fields[$i][] = $key;
        }
      }
    }
    return $this->fulltext_fields[$i];
  }

  /**
   * Helper function for creating an entity metadata wrapper appropriate for
   * this index.
   *
   * @return EntityMetadataWrapper
   *   A wrapper for the entity type of this index, optionally loaded with the
   *   given data, and having fields according to the data alterations of this
   *   index.
   */
  public function entityWrapper($item = NULL) {
    return entity_metadata_wrapper($this->entity_type, $item, array('property info alter' => array($this, 'propertyInfoAlter')));
  }

}