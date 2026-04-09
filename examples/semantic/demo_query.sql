DROP TABLE IF EXISTS objects;

CREATE TABLE objects (
    object_id INT PRIMARY KEY,
    name VARCHAR,
    length_cm INT,
    color VARCHAR
);

INSERT INTO objects (object_id, name, length_cm, color) VALUES
(1, 'Ruler', 30, 'White'),
(2, 'Pen', 8, 'Blue'),
(3, 'Eraser', 8, 'White');

SELECT
  o.name,
  o.length_cm,
  o.color
FROM
  objects AS o
WHERE
  SEMANTIC(
    'Object: name: {o.name}, length: {o.length_cm} cm, color: {o.color}. '
    'Is this object 8 cm long and white?'
  );
